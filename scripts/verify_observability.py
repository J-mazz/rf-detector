#!/usr/bin/env python3
"""Check live CSV/health visibility and output failures against the actual CLI."""
import argparse
import csv
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('--only', choices=('csv', 'health', 'errors'))
args = parser.parse_args()
exe = str(args.executable.resolve())

def stop(process):
    if process.poll() is None:
        process.terminate()
    try:
        return process.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise

with tempfile.TemporaryDirectory(prefix='rf-observability-') as tmp:
    root = Path(tmp)
    if args.only in (None, 'csv'):
        output = root / 'events.csv'
        process = subprocess.Popen([exe, '--seconds', '10', '--sample-rate', '10000000',
                                    '--bandwidth', '9000000', '--no-lock', '--events', str(output)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 2
            found = False
            while time.monotonic() < deadline and process.poll() is None:
                if output.exists():
                    # Only inspect complete newline-terminated records during writes.
                    text = output.read_text()
                    complete = text[:text.rfind('\n') + 1]
                    rows = list(csv.DictReader(complete.splitlines()))
                    found = any(row.get('phase') == '0' for row in rows)
                    if found:
                        break
                time.sleep(0.01)
            assert found and process.poll() is None, 'no begin event visible while process was running'
        finally:
            _, err = stop(process)
        assert process.returncode == 0, err
        print('live CSV visibility: PASS')

    if args.only in (None, 'health'):
        # Below one sample per second: unavailable reception must be observable
        # during operation, not just a final counter at shutdown.
        status = root / 'health.log'
        with status.open('wb') as log:
            process = subprocess.Popen([exe, '--seconds', '10', '--sample-rate', '0.1',
                                        '--bandwidth', '0.1', '--status-ms', '20',
                                        '--stall-ms', '100', '--no-lock'], stderr=log,
                                       stdout=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline and process.poll() is None:
                    if 'health state=unavailable' in status.read_text():
                        break
                    time.sleep(0.01)
                text = status.read_text()
                assert 'health state=starting' in text, text
                assert 'health state=unavailable' in text and process.poll() is None, text
            finally:
                stop(process)
        assert process.returncode == 0
        assert 'health state=stopped' in status.read_text()
        print('live unavailable/terminal health: PASS')

    if args.only in (None, 'errors'):
        for flag in ('--status-ms', '--stall-ms'):
            for value in ('0', '9', '60001', '1.5', 'nan'):
                run = subprocess.run([exe, flag, value, '--no-lock'], capture_output=True, timeout=3)
                assert run.returncode == 2, (flag, value, run.returncode)

        def small_file_limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))

        # The header fits; the first few event writes exhaust the file limit.
        run = subprocess.run([exe, '--seconds', '5', '--sample-rate', '10000000',
                              '--bandwidth', '9000000', '--no-lock',
                              '--events', str(root / 'full.csv')],
                             capture_output=True, timeout=3, preexec_fn=small_file_limit)
        assert run.returncode == 5, (run.returncode, run.stderr)
        assert b'health state=failed' in run.stderr, run.stderr
        def zero_file_limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

        # Failure while writing the header must reject output setup before
        # acquisition starts, even though creating the empty file succeeds.
        run = subprocess.run([exe, '--no-lock', '--events', str(root / 'header-full.csv')],
                             capture_output=True, timeout=3, preexec_fn=zero_file_limit)
        assert run.returncode == 2, (run.returncode, run.stderr)
        assert b'events output' in run.stderr and b'samples received=' not in run.stderr
        assert (root / 'header-full.csv').stat().st_size == 0
        print('option validation and output failure: PASS')

print('verify_observability: PASS')

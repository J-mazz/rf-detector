#!/usr/bin/env python3
"""Measure mock-source admission/loss; this is not a hardware or field benchmark."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import resource
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('--rates', type=float, nargs='+', default=[30.72e6, 10e6])
parser.add_argument('--seconds', type=float, default=5)
parser.add_argument('--repeat', type=int, default=1)
parser.add_argument('--bandwidth', type=float, help='default: min(sample rate, 30 MHz)')
parser.add_argument('--require-zero-loss', action='store_true')
args = parser.parse_args()
if not math.isfinite(args.seconds) or not 0 < args.seconds <= 3600 or not 1 <= args.repeat <= 100:
    parser.error('seconds must be in (0, 3600], repeat in [1, 100]')
if any(not math.isfinite(rate) or not 0 < rate <= 1e9 for rate in args.rates):
    parser.error('rates must be in (0, 1e9]')
if args.bandwidth is not None and (not math.isfinite(args.bandwidth) or not 0 < args.bandwidth <= min(args.rates)):
    parser.error('bandwidth must be positive and no greater than any requested rate')

sample_pattern = re.compile(r'^samples received=(\d+) admitted=(\d+) processed=(\d+) discarded=(\d+) unknown_gaps=(\d+)$', re.M)
event_pattern = re.compile(r'^windows=(\d+) invalid_windows=(\d+) resets=(\d+) events=(\d+) consumed=(\d+) dropped_events=(\d+)$', re.M)
terminal_pattern = re.compile(r'protocol_violations=(\d+) fatal=(\d+)$', re.M)
results = []
failed = False
binary = args.executable.resolve()
binary_sha256 = hashlib.sha256(binary.read_bytes()).hexdigest()
for rate in args.rates:
    for repetition in range(args.repeat):
        bandwidth = args.bandwidth if args.bandwidth is not None else min(rate, 30e6)
        command = [str(args.executable.resolve()), '--seconds', str(args.seconds), '--no-lock',
                   '--sample-rate', str(rate), '--bandwidth', str(bandwidth)]
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        started = time.monotonic()
        try:
            run = subprocess.run(command, capture_output=True, text=True, timeout=args.seconds + 15)
        except subprocess.TimeoutExpired:
            results.append({'command': command, 'repetition': repetition + 1, 'error': 'run/drain timed out'})
            failed = True
            continue
        elapsed = time.monotonic() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        samples = sample_pattern.search(run.stderr)
        events = event_pattern.search(run.stderr)
        terminal = terminal_pattern.search(run.stderr)
        if not samples or not events or not terminal:
            results.append({'command': command, 'exit_code': run.returncode, 'error': 'missing final summary', 'stderr': run.stderr})
            failed = True
            continue
        received, admitted, processed, discarded, gaps = map(int, samples.groups())
        windows, invalid, resets, emitted, consumed, dropped_events = map(int, events.groups())
        violations, fatal = map(int, terminal.groups())
        valid = (run.returncode == 0 and not fatal and not violations and
                 admitted == processed and received == admitted + discarded and emitted == consumed)
        # An idle or failed source must never pass a zero-loss assertion.
        zero_loss = valid and received > 0 and discarded == 0 and gaps == 0 and dropped_events == 0
        failed |= not valid or (args.require_zero_loss and not zero_loss)
        results.append({
            'command': command, 'repetition': repetition + 1, 'requested_rate_sps': rate,
            'elapsed_seconds_including_drain': elapsed, 'requested_seconds': args.seconds,
            'received_sps_including_drain': received / elapsed,
            'received_vs_requested_samples': received / (rate * args.seconds),
            'received': received, 'admitted': admitted,
            'processed': processed, 'discarded': discarded, 'unknown_gaps': gaps,
            'loss_fraction': discarded / received if received else None,
            'admitted_sps_including_drain': admitted / elapsed,
            'processed_sps_including_drain': processed / elapsed,
            'windows': windows, 'invalid_windows': invalid, 'resets': resets,
            'events': emitted, 'consumed': consumed, 'dropped_events': dropped_events,
            'cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
            'exit_code': run.returncode, 'protocol_violations': violations,
            'fatal': fatal, 'accounting_valid': valid, 'zero_loss': zero_loss,
        })
print(json.dumps({'scope': 'paced mock, host only; elapsed includes draining',
                  'executable': str(binary), 'executable_sha256': binary_sha256,
                  'runs': results}, indent=2))
raise SystemExit(1 if failed else 0)

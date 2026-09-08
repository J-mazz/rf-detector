#!/usr/bin/env python3
"""Exercise the actual CLI, lossless replay and CSV lifecycle across read sizes."""
import argparse
import csv
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('executable', type=Path)
p.add_argument('--reference', type=Path, help='also require identical CSV from this reference binary')
a = p.parse_args()
exe = str(a.executable.resolve())
with tempfile.TemporaryDirectory(prefix='rf-replay-check-') as tmp:
    root = Path(tmp)
    capture = root/'fixture.sc16'
    rng = random.Random(90210)
    with capture.open('wb') as f:
        for k in range(147456):
            phase = 2*math.pi*(512/8192)*k
            amplitude = 2000 if 49152 <= k < 98304 else 0
            f.write(struct.pack('<hh', round(rng.uniform(-120,120)+amplitude*math.cos(phase)),
                                round(rng.uniform(-120,120)+amplitude*math.sin(phase))))
    records = []
    for chunk in (127, 131072):
        output = root/f'events-{chunk}.csv'
        run = subprocess.run([exe, '--input', str(capture), '--read-size', str(chunk),
                              '--events', str(output), '--no-lock'],
                             capture_output=True, text=True, timeout=60, check=True)
        assert 'received=147456 admitted=147456 processed=147456 discarded=0 unknown_gaps=0' in run.stderr, run.stderr
        assert 'protocol_violations=0 fatal=0' in run.stderr, run.stderr
        with output.open() as f:
            rows = list(csv.DictReader(f))
        assert rows, 'fixture produced no observations'
        assert {row['phase'] for row in rows} >= {'0','2'}, rows
        ids = {row['event_id'] for row in rows}
        for event_id in ids:
            phases = [row['phase'] for row in rows if row['event_id']==event_id]
            assert phases[0]=='0' and phases[-1]=='2', phases
        records.append(rows)
        # An existing output must not be truncated, including a capture passed as output.
        before = output.read_bytes()
        refused = subprocess.run([exe, '--input', str(capture), '--events', str(output), '--no-lock'],
                                 capture_output=True, timeout=10)
        assert refused.returncode != 0 and output.read_bytes()==before
    assert records[0] == records[1], 'read fragmentation changed CSV observations'
    if a.reference:
        reference_output = root/'reference.csv'
        subprocess.run([str(a.reference.resolve()), '--input', str(capture), '--no-lock',
                        '--events', str(reference_output)], capture_output=True, check=True, timeout=60)
        with reference_output.open() as f:
            reference_rows = list(csv.DictReader(f))
        assert records[0] == reference_rows, 'CSV observations differ from reference binary'
        print('reference binary event equivalence: PASS')
    malformed = root/'malformed.sc16'
    malformed.write_bytes(b'123')
    assert subprocess.run([exe, '--input', str(malformed), '--no-lock'], capture_output=True, timeout=10).returncode != 0
    assert subprocess.run([exe, '--fft', '1000', '--no-lock'], capture_output=True, timeout=10).returncode != 0
    print(f'verify_replay: PASS ({len(records[0])} identical event records; 147456 samples per read size)')

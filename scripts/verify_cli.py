#!/usr/bin/env python3
"""Exercise CLI parsing, setup errors, and accepted boundaries without hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    executable = str(args.executable.resolve())
    checks = 0

    def run(options, expected, diagnostic=None):
        nonlocal checks
        result = subprocess.run([executable, *options], capture_output=True,
                                text=True, timeout=3)
        assert result.returncode == expected, (options, result.returncode, result.stderr)
        if diagnostic:
            assert diagnostic in result.stdout + result.stderr, (options, result.stdout, result.stderr)
        checks += 1

    run(["--help"], 0, "Raw files contain little-endian")
    run(["--unknown", "1"], 2, "unknown option")
    numeric = ("--seconds", "--status-ms", "--stall-ms", "--frequency", "--sample-rate",
               "--bandwidth", "--gain", "--full-scale", "--threshold", "--read-size", "--fft")
    for option in (*numeric, "--input", "--device", "--events"):
        run([option], 2, "missing value")
    for option in numeric:
        for value in ("", "abc", "nan", "inf", "-inf", "1x", "1e999", "1e-999"):
            run([option, value], 2, "invalid numeric value")
    invalid = {
        "--seconds": ("-1",), "--frequency": ("0", "-1"),
        "--sample-rate": ("0", "-1", "1000000001"),
        "--bandwidth": ("0", "-1", "30720001"), "--gain": ("1e100",),
        "--full-scale": ("0", "0.5", "32769"),
        "--read-size": ("0", "-1", "1.5", "131073"),
        "--fft": ("255", "65537", "256.5"),
        "--status-ms": ("9", "60001", "10.5"), "--stall-ms": ("9", "60001", "10.5"),
    }
    for option, values in invalid.items():
        for value in values:
            run([option, value, "--no-lock"], 2)
    with tempfile.TemporaryDirectory(prefix="rf-cli-") as temp:
        root = Path(temp)
        empty = root / "empty.sc16"
        empty.touch()
        base = ["--input", str(empty), "--no-lock"]
        run([*base, "--device", "driver=fake"], 2, "choose either")
        run(["--input", str(root / "missing"), "--no-lock"], 4, "receiver start failed")
        run(["--input", str(root), "--no-lock"], 4, "receiver start failed")
        run([*base, "--events", str(root / "missing" / "events.csv")], 2, "events output")
        for option, value in (("--fft", "257"), ("--threshold", "6"), ("--threshold", "1e100")):
            run([*base, option, value], 3, "initialization failed")
        for size in (1, 2, 3, 5, 6, 7):
            malformed = root / f"bad-{size}.sc16"
            malformed.write_bytes(bytes(size))
            run(["--input", str(malformed), "--no-lock"], 4, "receiver start failed")
        # EOF makes even --seconds 0 deterministic and checks boundary values
        # without relying on a timed or high-throughput mock run.
        valid = (("--seconds", "0"), ("--read-size", "1"), ("--read-size", "131072"),
                 ("--fft", "256"), ("--fft", "65536"), ("--full-scale", "1"),
                 ("--full-scale", "32768"), ("--gain", "-10"),
                 ("--status-ms", "10"), ("--status-ms", "60000"),
                 ("--stall-ms", "10"), ("--stall-ms", "60000"))
        for option, value in valid:
            run([*base, option, value], 0, "health state=stopped")
        output = root / "events.csv"
        run([*base, "--unpaced", "--events", str(output)], 0, "samples received=0 admitted=0 processed=0 discarded=0")
        assert len(output.read_text().splitlines()) == 1
        sentinel = root / "sentinel.csv"
        sentinel.write_text("preserve me\n")
        link = root / "link.csv"
        link.symlink_to(sentinel)
        run([*base, "--events", str(link)], 2, "events output")
        assert sentinel.read_text() == "preserve me\n" and link.is_symlink()
    print(f"verify_cli: PASS ({checks} CLI cases)")


if __name__ == "__main__":
    main()

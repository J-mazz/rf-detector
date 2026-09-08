#!/usr/bin/env python3
"""Exercise bounded acquisition shutdown behavior through the rfdet CLI."""

import argparse
import signal
import subprocess
import time
from pathlib import Path


DEADLINE_SECONDS = 2.0


def common_args(sample_rate: str) -> list[str]:
    return [
        "--sample-rate",
        sample_rate,
        "--bandwidth",
        sample_rate,
        "--no-lock",
    ]


def run_timed(binary: Path, sample_rate: str) -> None:
    command = [str(binary), *common_args(sample_rate), "--seconds", "0.1"]
    started = time.monotonic()
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=DEADLINE_SECONDS,
        check=False,
    )
    elapsed = time.monotonic() - started
    if completed.returncode != 0:
        raise RuntimeError(
            f"timed {sample_rate} SPS run exited {completed.returncode}:\n{completed.stderr}"
        )
    print(f"PASS timed sample_rate={sample_rate} elapsed={elapsed:.3f}s")


def run_sigterm(binary: Path) -> None:
    command = [str(binary), *common_args("1000")]
    child = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        time.sleep(0.2)
        if child.poll() is not None:
            stdout, stderr = child.communicate()
            raise RuntimeError(
                f"SIGTERM target exited early with {child.returncode}:\n{stdout}{stderr}"
            )
        started = time.monotonic()
        child.send_signal(signal.SIGTERM)
        stdout, stderr = child.communicate(timeout=DEADLINE_SECONDS)
        elapsed = time.monotonic() - started
    except BaseException:
        child.kill()
        child.communicate()
        raise
    if child.returncode != 0:
        raise RuntimeError(
            f"SIGTERM run exited {child.returncode}:\n{stdout}{stderr}"
        )
    print(f"PASS SIGTERM sample_rate=1000 elapsed={elapsed:.3f}s")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binary",
        nargs="?",
        type=Path,
        default=Path("build-gcc16/rfdet"),
        help="rfdet executable (default: build-gcc16/rfdet)",
    )
    parser.add_argument(
        "--only",
        choices=("low-rate-timed", "sub-hz-timed", "low-rate-sigterm"),
        help="run one acquisition regression",
    )
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"binary not found: {binary}")

    cases = {
        "low-rate-timed": lambda: run_timed(binary, "1000"),
        "sub-hz-timed": lambda: run_timed(binary, "0.25"),
        "low-rate-sigterm": lambda: run_sigterm(binary),
    }
    if args.only:
        cases[args.only]()
    else:
        for case in cases.values():
            case()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Report project line coverage from a GCC --coverage build after running tests.

Example: python3 scripts/report_coverage.py build-coverage
Only instrumented executable lines in src/ are counted. This does not measure
branch coverage or code excluded by compile-time options (such as NEON/Soapy).
"""
import argparse
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--gcov", default="gcov", help="gcov matching the build compiler")
    parser.add_argument("--missing", action="store_true", help="list uncovered source lines")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    objects = sorted(args.build.resolve().rglob("*.gcno"))
    if not objects:
        parser.error("no .gcno files found; build with --coverage first")
    lines = {}
    missing_data = 0
    with tempfile.TemporaryDirectory(prefix="rf-gcov-") as temp:
        for index, obj in enumerate(objects):
            work = Path(temp) / str(index)
            work.mkdir()
            result = subprocess.run([args.gcov, "--json-format", str(obj)], cwd=work,
                                    check=True, capture_output=True, text=True, timeout=30)
            for diagnostic in result.stderr.splitlines():
                if diagnostic.endswith(":cannot open data file, assuming not executed"):
                    missing_data += 1
                else:
                    print(diagnostic, file=sys.stderr)
            for report in work.glob("*.gcov.json.gz"):
                with gzip.open(report, "rt") as stream:
                    data = json.load(stream)
                for file in data["files"]:
                    path = Path(file["file"])
                    if not path.is_absolute():
                        path = Path(data["current_working_directory"]) / path
                    path = path.resolve()
                    if not path.is_relative_to(root / "src"):
                        continue
                    counts = lines.setdefault(str(path.relative_to(root)), {})
                    for line in file["lines"]:
                        number = line["line_number"]
                        counts[number] = counts.get(number, 0) + line["count"]
    if not lines:
        parser.error("no project source lines found in gcov output")
    total = covered = 0
    for path, counts in sorted(lines.items()):
        hit = sum(count > 0 for count in counts.values())
        total += len(counts)
        covered += hit
        print(f"{path:25} {hit:4}/{len(counts):4} {100 * hit / len(counts):6.2f}%")
        if args.missing:
            print("  uncovered:", ", ".join(str(n) for n, c in sorted(counts.items()) if not c))
    print(f"{'TOTAL':25} {covered:4}/{total:4} {100 * covered / total:6.2f}%")
    if missing_data:
        print(f"Note: {missing_data} objects lack execution data; any source lines they contain count as unexecuted unless covered by another object.")


if __name__ == "__main__":
    main()

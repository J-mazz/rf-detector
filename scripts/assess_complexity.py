#!/usr/bin/env python3
"""Run CodeScene reviews with a separately labeled, optional Lizard CCN inventory.

Requires an authenticated CodeScene CLI. Exit 2 means CodeScene is incomplete;
local metrics never turn a blocked CodeScene assessment into a passing result.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def inventory():
    paths = [*ROOT.glob("src/**/*.cppm"), *ROOT.glob("src/**/*.cpp"),
             *ROOT.glob("tests/test_*.cpp"), *ROOT.glob("scripts/*.py")]
    paths.sort(key=lambda path: ({"src": 0, "tests": 1, "scripts": 2}[path.relative_to(ROOT).parts[0]], str(path)))
    return [{"path": str(path.relative_to(ROOT)),
             "parser_name": str(path.relative_to(ROOT).with_suffix(".cpp")) if path.suffix == ".cppm" else str(path.relative_to(ROOT)),
             "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in paths]


def local_metrics(files):
    try:
        import lizard
    except ImportError:
        return {"status": "unavailable", "reason": "Optional lizard package is not installed"}
    functions = []
    for file in files:
        parsed = lizard.analyze_file.analyze_source_code(file["parser_name"], (ROOT / file["path"]).read_text())
        for function in parsed.function_list:
            functions.append({"path": file["path"], "function": function.name,
                              "start_line": function.start_line, "end_line": function.end_line,
                              "ccn": function.cyclomatic_complexity, "nloc": function.nloc,
                              "parameters": function.parameter_count})
    functions.sort(key=lambda item: (-item["ccn"], item["path"], item["start_line"]))
    return {"status": "complete", "tool": "Lizard", "version": importlib.metadata.version("lizard"),
            "metric": "Lizard CCN; not CodeScene scores or findings", "functions": functions}


def invoke(command, environment, source=None):
    try:
        result = subprocess.run(command, cwd=ROOT, env=environment, input=source,
                                capture_output=True, text=True, timeout=60)
        return {"exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired:
        return {"exit_code": None, "stdout": "", "stderr": "CodeScene timed out after 60 seconds"}
    except OSError as error:
        return {"exit_code": None, "stdout": "", "stderr": str(error)}


def codescene_reviews(executable, files):
    binary = shutil.which(executable)
    if not binary:
        return {"status": "blocked", "reason": "CodeScene executable not found", "reviews": []}
    environment = {**os.environ, "CS_DISABLE_VERSION_CHECK": "1"}
    version = invoke([binary, "version"], environment)
    report = {"status": "complete", "version": version, "reviews": []}
    for file in files:
        # Only the filename hint changes. Preserve module syntax, source text,
        # and line numbers; CodeScene must parse the actual working-tree code.
        command = [binary, "review", "--output-format", "json", "--file-name", file["parser_name"]]
        result = invoke(command, environment, (ROOT / file["path"]).read_text())
        result["path"] = file["path"]
        report["reviews"].append(result)
        if result["exit_code"] != 0:
            report["status"] = "blocked"
            report["reason"] = "CodeScene review failed; see the recorded diagnostic. Remaining files were not reviewed."
            break
        try:
            result["review"] = json.loads(result["stdout"])
        except json.JSONDecodeError:
            report["status"] = "blocked"
            report["reason"] = "CodeScene returned non-JSON output; no successful assessment is claimed."
            break
        del result["stdout"]
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codescene", default="cs", help="CodeScene executable path")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/quality/complexity.json")
    args = parser.parse_args()
    files = inventory()
    report = {"generated_at": datetime.now(timezone.utc).isoformat(),
              "scope": "Working-tree src C++, unit-test C++, and Python scripts; generated builds and API shim headers excluded",
              "module_handling": "Unmodified .cppm content with .cpp filename hint for both parsers",
              "files": files, "codescene": codescene_reviews(args.codescene, files),
              "local_complexity": local_metrics(files)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"CodeScene: {report['codescene']['status']}; report: {args.output}")
    local = report["local_complexity"]
    if local["status"] == "complete":
        print(f"Supplemental Lizard {local['version']}: {len(local['functions'])} functions")
        for function in [f for f in local["functions"] if f["path"].startswith("src/")][:10]:
            print(f"  CCN {function['ccn']:2} {function['path']}:{function['start_line']} {function['function']}")
    return 0 if report["codescene"]["status"] == "complete" else 2


if __name__ == "__main__":
    raise SystemExit(main())

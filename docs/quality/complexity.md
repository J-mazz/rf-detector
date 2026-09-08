# Complexity assessment — 2026-09-08

**CodeScene completed 34 file reviews**, producing numeric Code Health scores for 32 files. The authenticated run finished at 2026-09-08 11:18:48 UTC using CLI 1.0.40. All 34 recorded source hashes match the working tree. Two Python scripts returned null scores, which are treated as unscored rather than healthy.

The [raw report](complexity.json) preserves CodeScene's actual scores, findings, tool version, source hashes, and a separate Lizard 1.23.0 inventory. The [compact CodeScene summary](codescene-summary.json) extracts the per-file scores and reported complex methods without mixing in Lizard values. Scope: 13 production C++ files, 12 C++ test files, and nine Python scripts, including uncommitted changes. Generated builds and Soapy API declaration headers are excluded.

## CodeScene results

CodeScene flags **13 production functions** as Complex Method. The largest reported cyclomatic complexities are:

| Function | Source | CodeScene CC | Lizard CCN | File Code Health / 10 |
|---|---|---:|---:|---:|
| `main` | `src/main.cpp:58` | 60 | 66 | 7.00 |
| `StreamingDetector::initialize` | `src/rf/dsp.cppm:43` | 42 | 42 | 4.05 |
| `StreamingDetector::analyze` | `src/rf/dsp.cppm:170` | 41 | 41 | 4.05 |
| `StreamingDetector::feed` | `src/rf/dsp.cppm:86` | 29 | 29 | 4.05 |
| `PipelineSystem::run_acquisition` | `src/rf/pipeline.cppm:69` | 27 | 21 | 7.78 |
| `Monitor::poll` | `src/rf/health.cppm:49` | 23 | 23 | 9.13 |
| `PinnedRegion::map` | `src/rf/memory.cppm:128` | 15 | 16 | 9.24 |
| `valid_config` | `src/rf/acquire.cppm:37` | 12 | 12 | 9.66 |
| `PipelineSystem::run_dsp` | `src/rf/pipeline.cppm:122` | 12 | 11 | 7.78 |

Code Health is a file-level composite score, not a function CC score or a test-coverage percentage. CodeScene's review recommends C++ function complexity below 9. This is an advisory review threshold, not a new project gate, and the table demonstrates why CodeScene and Lizard values must remain distinct.

`dsp.cppm` is the lowest-scoring production file at **4.05/10**. CodeScene reports six nested-logic clusters in `analyze`, a complex validation expression in `initialize` at line 47 with a reported count of 28, and another in `feed` at line 87 with a count of 14. It also flags four complex methods in this file and the five-argument `publish` function.

`main.cpp` scores **7.00/10**, with CC 60 and eleven nested-logic clusters in `main`. `pipeline.cppm` scores **7.78/10**: acquisition, DSP, and initialization are flagged as complex methods; acquisition has four nested-logic clusters. These support reviewing the detector, CLI orchestration, and pipeline first.

### Test and script findings

CodeScene flags 15 complex test functions. The largest are `test_dsp.cpp::main` (CC 30), `test_health.cpp::main` (24), and `test_acquire.cpp::main` plus two Soapy test functions (21 each). The lowest-scoring test files are `test_soapy.cpp` (7.80/10) and `test_pipeline.cpp` (8.08/10). Both contain large assertion blocks. Test-only Soapy API shim functions are also flagged for argument counts dictated by the emulated C API; those signatures should remain compatible with that API.

For Python functions, `report_coverage.py::main` has CC 17, three nested-logic clusters, and nesting depth four; its file score is 8.80/10. `verify_cli.py::main` has CC 11 and its file score is 9.53/10. These findings favor named scenario/helper functions for readability, while preserving the assertions and reporting semantics.

### Scoring and parser limits

- `benchmark_runtime.py` and `verify_replay.py` return `score: null` with no findings. They contain module-level executable logic and no named functions. This is consistent with a function-oriented scoring limitation, but the cause is not established by the returned JSON. Neither is counted as a 10/10 result.
- CodeScene reports `soapy.cppm` as 10/10 without findings, while Lizard reports `SoapySource::configure` at CCN 23 and `read_into` at 12. CodeScene also reports only `valid_config` as a complex method in `acquire.cppm`, while Lizard flags `MockSource::read_into` at 21. These discrepancies need focused parser/preprocessor/override-method validation; absence of findings does not establish that those methods are simple or fully recognized.
- The `.cppm` files were submitted unchanged with `.cpp` filename hints. Correct function names and source locations in the DSP, pipeline, FFT, and health findings confirm useful recognition of those module bodies. This does not prove complete parsing of every C++26 construct or build configuration.
- This is a working-tree assessment, not a CodeScene delta analysis. It does not show whether the test expansion improved or worsened Code Health. The high production complexities were already present; the coverage work did not change production sources.

## Supplemental local results: Lizard, not CodeScene

| Production function | Source | Lizard CCN | Function code lines |
|---|---|---:|---:|
| `main` | `src/main.cpp:58` | 66 | 102 |
| `StreamingDetector::initialize` | `src/rf/dsp.cppm:43` | 42 | 37 |
| `StreamingDetector::analyze` | `src/rf/dsp.cppm:170` | 41 | 65 |
| `StreamingDetector::feed` | `src/rf/dsp.cppm:86` | 29 | 24 |
| `Monitor::poll` | `src/rf/health.cppm:49` | 23 | 51 |
| `SoapySource::configure` | `src/rf/soapy.cppm:53` | 23 | 66 |
| `MockSource::read_into` | `src/rf/acquire.cppm:72` | 21 | 46 |
| `PipelineSystem::run_acquisition` | `src/rf/pipeline.cppm:69` | 21 | 51 |
| `PinnedRegion::map` | `src/rf/memory.cppm:128` | 16 | 33 |

Production contains 149 recognized functions, mean CCN 4.23; nine exceed Lizard's default warning threshold of 15. Tests contain 99 recognized functions, mean CCN 4.51; nine exceed 15. Python scripts contain 17 recognized functions, mean CCN 4.00; one exceeds 15. Python module-level execution is outside this function inventory.

The largest test functions are `test_dsp.cpp::main` (31), `test_health.cpp::main` (24), and `test_acquire.cpp::main` (21). `configuration_failures_and_retry` in the Soapy shim also scores 21. These counts describe assertion/control-flow structure, so review them separately from production. `report_coverage.py::main` scores 17.

## Interpretation and priorities

High line coverage does not establish low complexity or exercise every decision outcome. The earlier 99.28% line-coverage result is compatible with the high function CCNs above. In particular, the detector validation chains pack many decisions into few source lines.

1. `main` combines CLI parsing, setup, worker lifecycle, and reporting. It is the clearest candidate for separating responsibilities while retaining the new CLI boundary tests.
2. `StreamingDetector::analyze` combines input validation, FFT preparation, reference estimation, calibration, and event transitions. Review these as cohesive operations; any refactoring must preserve numerical boundaries, fragmentation equivalence, and throughput.
3. Detector `initialize` and `feed` deserve decision/condition coverage in addition to line coverage. Their large CCNs partly reflect explicit validation and continuity comparisons; reducing a metric by merely hiding these expressions would not establish simpler behavior.
4. Acquisition, receiver setup, and health priority logic should retain their failure-accounting and ordering tests. Their complexity includes necessary error handling, not just removable branching.
5. The larger test `main` functions can be divided into named scenario functions to improve failure localization. This is an assessment recommendation, not a production refactoring performed in this change.

The priorities above are interpretations of CodeScene findings, the local inventory, and source code. [Lizard documents](https://github.com/terryyin/lizard) its source-based CCN analysis; it does not require full compilation. Both compile-time branches can contribute, and macro expansion/compiler control-flow graphs are not analyzed here. Do not equate its CCNs with CodeScene's algorithm, Code Health score, or coverage percentages.

## Reproduce the CodeScene assessment

The downloaded CLI is `/tmp/rf-codescene/cs` (temporary installation). The successful run used the user's authenticated terminal with `CS_ACCESS_TOKEN` exported. Run the assessment in that same environment: environment variables exported in another shell are not inherited by an already-running agent session. Enterprise installations also need `CS_ONPREM_URL`. Do not place credentials in the repository or report. See the official [CLI authentication guide](https://docs.enterprise.codescene.io/latest/cli/index.html) and [command reference](https://docs.enterprise.codescene.io/latest/cli/command-reference.html).

```sh
python3 scripts/assess_complexity.py --codescene /tmp/rf-codescene/cs
```

For an existing installation on PATH:

```sh
python3 scripts/assess_complexity.py --codescene cs
```

The CLI's `cs docs file-name` lists C++ extensions but omits `.cppm`. The runner supplies unmodified module text on stdin with a `.cpp` filename hint, retaining original paths and hashes in the report. The hint would also affect extension-specific custom rules, so the report records it explicitly. See the scoring and parser limits above.

The runner exits 2 when CodeScene is unavailable, authentication fails, a review fails, or output is not JSON. Supplemental metrics never change that status. Exit zero means all review commands succeeded, not that all files were scored or are healthy. CodeScene reports flagged complexity issues rather than a CCN for every function; the separate Lizard inventory provides its full recognized-function list. A rerun replaces the report, so use `--output` with another path for experimental or unauthenticated attempts. The compact summary is derived from this recorded successful scan.

Runner checks exercised all 34 filename/source routes against a clearly identified test double, plus missing-executable and malformed-output failures. The actual authenticated CodeScene report subsequently completed all 34 commands with exit zero and returned valid JSON. Report verification checked all source hashes, distinguished the two null scores, and checked the compact summary against the raw findings.

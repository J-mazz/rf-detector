# Runtime Reliability and Throughput Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task by task. Track completion below.

**Goal:** Fix bounded mock shutdown, timely CSV publication and observable receiver health, then improve and measure DSP throughput.

**Architecture:** Keep the three worker stages and fixed pools. Separate control-plane health interpretation and CSV ownership from main; retain the FFT backend while removing redundant hot-loop work.

**Tech Stack:** Linux/POSIX, GCC 16 C++26 modules/import std, CMake/Ninja, Python CLI integration tests.

**Spec:** `docs/superpowers/specs/2026-09-07-runtime-refactor-design.md`

## Global Constraints

- Native GCC 16 C++26 `import std` remains the primary verified build.
- Capture/DSP storage remains allocated before workers start; drain admitted work on stop.
- Preserve the existing CSV schema, sample accounting, FFT normalization and event lifecycle.
- No new third-party library, receiver hardware, classifier, GUI, deployment or field-effectiveness claim is included.
- Use current working files, including untracked sources from the authorized flattening. Do not commit unrelated existing changes or create a worktree missing those files.

## Task 1: Bound mock acquisition pacing

**Files:** `src/rf/acquire.cppm`, `tests/test_acquire.cpp`, `scripts/verify_runtime.py` (acquisition cases only).
**Interface:** Existing `ReadResult MockSource::read_into(int16_t*, size_t, uint32_t timeout_us) noexcept`; no interface changes.

- [x] Add a CLI test that invokes `--sample-rate 1000 --bandwidth 1000 --seconds 0.1 --no-lock` with a two-second subprocess deadline and requires exit zero; add a low-rate SIGTERM test and a sub-Hz timed-run case.
- [x] Run against `build-review-baseline/rfdet`; confirm timed shutdown fails. Unit-test timeout without sample-clock advancement and successful subsequent timestamps, preserving deterministic unpaced samples.
- [x] Implement deadline-limited pacing: compute samples available within the read timeout using elapsed monotonic time, clamp to capacity, and return zero-sample timeout if no full sample is due. Use bounded sleeps; avoid undefined floating/integer conversions and timestamp overflow for valid rates.
- [x] Run acquire tests and CLI shutdown cases; record evidence.

## Task 2: Expose live health and reliable CSV publication

**Files:** `src/rf/runtime.cppm`, `src/rf/pipeline.cppm`, new `src/rf/health.cppm`, new `src/rf/output.cppm`, `src/main.cpp`, new `tests/test_health.cpp`, `tests/test_pipeline.cpp`, `scripts/verify_observability.py`, build scripts/CMake/portable module order.
**Interfaces:** `StageState` adds atomic capture/progress timestamps and detector health; `rf::health::Monitor(start_ns, stall_ns).poll(const StageState&, now_ns)` returns an immutable snapshot. `rf::output::CsvWriter` owns the exclusive-create stream and supplies a pipeline callback.

- [x] Extend the CLI regression to read a nonempty CSV event row while the child is still alive; the existing fully buffered binary must fail. Require unchanged output-file protection and prompt fatal handling on write failures.
- [x] Add unit tests with explicit monotonic timestamps: starting -> tracking -> unavailable -> recovery; backlog without progress; overload/gap/invalid-event intervals; calibration; failed/stopping/stopped; a timestamp newer than the poll timestamp must not underflow.
- [x] Add pipeline tests using a finite fake source that produces timeouts/overflows and then samples; verify live timestamps/counters and retained ownership after drain.
- [x] Implement atomics and per-frame DSP telemetry updates. Build the health observer without accessing DSP-owned state directly. Add status interval/stall CLI parsing and periodic/terminal stderr lines with counters and flags.
- [x] Extract CSV ownership, set line buffering before the checked header write, and check record and close errors. Keep the sink on its own worker and the schema unchanged.
- [x] Register new modules/tests in canonical, CMake and portable paths. Run unit tests, replay checks, runtime integration and existing tests.

## Task 3: Reduce redundant DSP work and measure capacity

**Files:** `src/rf/dsp.cppm`, `tests/test_dsp.cpp`, new `scripts/benchmark_runtime.py`.
**Interface:** `StreamingDetector` API and CandidateEvent schema unchanged; benchmark consumes existing CLI and final sample-summary lines.

- [x] Preserve the old executable in `build-review-baseline/rfdet`. Run the existing FFT/DSP/replay checks before optimization. Add meaningful tests for near-threshold hysteresis and selected-band/frequency behavior if the changed path lacks coverage.
- [x] Replace runtime modulo by masks where validated FFT sizes are powers of two. Avoid unnecessary logarithms for clearly subthreshold bands, retaining exact dB comparison near the threshold; use a bounded tiny-array sort to avoid the existing optimizer warnings. Cache only configuration/segment invariants that are proven not to change during a feed segment.
- [x] Run DSP/FFT/replay tests and verify event identity/positions/values across fragmentation. Confirm warnings from modified code are resolved.
- [x] Add a Python benchmark invoking a binary sequentially at configured rates/durations, parsing summaries and reporting JSON with received/admitted/processed/discarded/gaps/elapsed. `--require-zero-loss` rejects loss, gaps, incomplete processing or fatal execution. Avoid timing-based CI pass assertions for hardware performance.
- [x] Compare old/new five-second 30.72 MS/s runs and a 10 MS/s run while no builds run; record measured capacity and remaining loss honestly.

## Task 4: Integration review and documentation

**Files:** README.md, DESIGN.md, REVIEW.md, VALIDATION.md, this plan.

- [x] Run native GCC 16 build and every test, replay/runtime CLI scripts, CMake/CTest, native Soapy shim and available sanitizers. Re-run only checks affected by subsequent fixes.
- [x] Obtain independent code/spec review of changed behavior, fix material issues and recheck affected tests.
- [x] Update runtime/health/output contracts, reproducible commands and measured performance. Mark original review findings fixed/mitigated/open with evidence; retain hardware/classification gates.
- [x] Finish with a concise outcome, commands, validation and remaining limits. Leave changes reviewable in the working tree.

## Execution record

- Ruling: work in the current tree — the user requested implementation here and the full project is still untracked after flattening; creating a HEAD-only worktree would omit it.
- Ruling: proceed after plan creation — the user explicitly authorized plan followed by implementation, so no additional approval gate is needed.
- Shared-file check: Tasks 1/2 keep separate CLI scripts: verify_runtime.py for acquisition and verify_observability.py for output/health, avoiding concurrent edits. Tasks 2/3 share no implementation files. Task 4 reviews/integrates all modules and build registrations.
- Consistency check: each task retains existing public contracts except the additive health API and flags; no task requires new hardware or an external dependency.

- Task 1 implementation and native integration checks pass: timed 1000 SPS 0.211s, timed 0.25 SPS 0.212s, SIGTERM 0.112s. Independent final review passed.
- Task 2 native health/pipeline tests, live CSV, unavailable/terminal status, output failure and replay checks pass. Independent task review passed after the fixes below.
- Task 3 DSP implementation completed and independently reviewed; benchmark script reports actual rate and loss.

- Task 2 review found missing live reset serialization and idle-time DSP false stalls. Both now have red/green regression evidence and passed scoped independent re-review. Monitor starts a DSP deadline only on observed pending work and restarts on observed progress.
- Integration: CMake header/module build and all 11 CTest cases pass (eight unit executables plus replay/runtime/observability CLI checks). Direct <atomic>/<cstdint> includes were added after the fallback build exposed missing imports.
- Sanitizer runtime probes cannot link: GCC ASan and TSan shared libraries are absent. No sanitizer execution is claimed.

- Task 1 independent review: spec and quality PASS; no material defects. Nonblocking observations retained: wall-clock test deadlines can fail under extreme scheduler stalls, and the SIGTERM test currently uses a startup delay.

- Task 3 measured DSP-only improvement: old 18–21 MS/s, optimized 24–27 MS/s; default-rate loss remained 10–20%. Ruling: extend the performance task to Pipeline Backoff only — repeated sched_yield on empty queues consumes a core, so test a 50 microsecond idle sleep after 64 spins. This adds a bounded polling delay plus scheduler latency while leaving queued work/draining unchanged. Compare before any compiler optimization-level change.

- Final integration after the latest DSP boundary tests and backoff: warning-free canonical GCC 16 C++26/import-std and CMake Release builds; eight native units, three CLI scripts, exact original-binary replay equivalence, native Soapy C API shim, and 11/11 CTest checks PASS. Shell/Python syntax and whitespace checks pass. Sanitizers remain unavailable, not passed.
- Independent final code/spec review and the scoped 50 microsecond backoff review PASS with no material defects. Nonblocking future coverage: direct same-detector sample-rate/bandwidth transitions and isolated close-only CSV failure injection.
- Final five-second default-rate runs processed 28.77 and 28.50 MS/s including drain, losing 5.07% and 5.89% of received samples. Original comparison runs processed 17.99 and 20.99 MS/s with 40.27% and 30.37% loss. Final CPU time was about 5.8 seconds versus 11.5 seconds per run. A final 10 MS/s run processed all 50,174,345 samples and passed the zero-loss assertion. Retained the canonical -O2 setting; hardware/target capacity and latency are not established.
- README, DESIGN, REVIEW and VALIDATION now describe delivered behavior, fixed/mitigated/open findings and reproducible raw benchmark artifacts. Historical findings/checkpoints are labeled. All authorized refactor tasks are complete; changes remain reviewable in the current working tree without a commit or deployment.

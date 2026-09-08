# Runtime refactor validation — 2026-09-07

The refactor plan is in [docs/superpowers/plans/2026-09-07-runtime-refactor.md](docs/superpowers/plans/2026-09-07-runtime-refactor.md). Host/toolchain: Linux x86-64, Intel Core Ultra 9 185H, GCC 16.2.1; CMake 4.3.0 / Ninja.

## Regressions reproduced and fixed

- All three acquisition regressions failed against the preserved original binary at a two-second deadline. Native rebuilt runs passed: 1000 SPS timed stop in 0.211 s, 0.25 SPS timed stop in 0.212 s, and 1000 SPS SIGTERM stop in 0.112 s. Paced timeout does not consume samples; unrepresentable mock timestamps are explicitly invalid.
- Original CSV buffering failed the live-begin-record visibility test. The line-buffered writer passes it while the child is running, preserves replay records and existing-file protection, and detects a 512-byte output-file limit as a fatal write failure.
- Health tests cover starting/calibrating/tracking/degraded/unavailable/recovery and terminal states, interval versus cumulative loss, reset counters, future timestamp clamping and idle-then-backlog behavior. Source timeouts/overflows do not publish successful-capture timestamps. DSP window telemetry is checked before workers stop.
- Independent Task 2 review found two issues in the first implementation: omitted reset serialization and idle time counted as a DSP stall. Both have red/green tests and passed scoped re-review. Acquisition review found no material defects.

## Verification commands

Final verification after the DSP boundary tests and idle-backoff change: both builds completed without warnings; all eight native unit executables, all three CLI scripts, the native Soapy shim and 11/11 CTest checks passed. The replay fixture also matched the preserved original binary exactly: 10 event records, 147,456 samples per fragmentation case. Final timed shutdowns took 0.205 s at 1000 SPS and 0.25 SPS; SIGTERM shutdown took 0.106 s at 1000 SPS. Independent final code/spec and scoped backoff reviews found no material defects.

```sh
CXX=/usr/bin/g++ ./build.sh --out build-gcc16
for test in build-gcc16/test_*; do "$test" || exit; done
python3 scripts/verify_replay.py build-gcc16/rfdet --reference build-review-baseline/rfdet
python3 scripts/verify_runtime.py build-gcc16/rfdet
python3 scripts/verify_observability.py build-gcc16/rfdet
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build build-cmake -j 4
ctest --test-dir build-cmake --output-on-failure
```

CMake now registers eight unit executables and three Python CLI checks (11 checks when Python is available). Native Soapy module compilation and the C API test shim also pass after acquisition changes. The real SoapySDR SDK/device are still absent.

GCC sanitizer probes cannot link: `/usr/lib64/libasan.so.8.0.0` and `/usr/lib64/libtsan.so.2.0.0` are missing. No sanitizer execution is claimed for this refactor. Unit-test timing bounds allow normal scheduling overhead but are not hard real-time guarantees under arbitrary host stalls.

The benchmark's `--require-zero-loss` failure path was exercised against the old binary: a one-second 30.72 MS/s run lost 14,863,560 of 31,142,697 samples, reported valid accounting but `zero_loss: false`, and exited 1. This is a check of loss detection, not a controlled performance comparison.

## Final measured capacity

Canonical GCC 16 C++26/import-std `-O2` binaries, default FFT/hop, paced mock, `--no-lock`, no CSV sink, sequential five-second runs without concurrent project builds or benchmarks. The final binary retains `-O2`; no compiler optimization-level change is included. Elapsed processing rates include draining. Normal host scheduling/thermal variation remains uncontrolled, and these short runs do not establish a sustained operating envelope.

| Stage / input | Received | Processed | Discarded (%) | Processed MS/s | CPU seconds |
|---|---:|---:|---:|---:|---:|
| Original / 30.72 MS/s, run 1 | 153,747,405 | 91,829,010 | 61,918,395 (40.27%) | 17.99 | 11.50 |
| Original / 30.72 MS/s, run 2 | 153,878,477 | 107,138,215 | 46,740,262 (30.37%) | 20.99 | 11.48 |
| Final / 30.72 MS/s, run 1 | 154,009,549 | 146,197,658 | 7,811,891 (5.07%) | 28.77 | 5.83 |
| Final / 30.72 MS/s, run 2 | 153,668,762 | 144,624,794 | 9,043,968 (5.89%) | 28.50 | 5.82 |
| Final / 10 MS/s, 9 MHz bandwidth | 50,174,345 | 50,174,345 | 0 (0%) | 9.98 | 2.34 |

All rows have consistent sample accounting, zero unknown gaps, event drops, fatal errors or ownership violations. Final default-rate runs had 59 and 69 continuity resets; the 10 MS/s run had none and passed `--require-zero-loss`. Default-rate sample loss remains a product blocker despite improvement. Idle backoff trades repeated yields for a requested 50 microsecond sleep after 64 pauses, adding polling/scheduler latency; alert latency still needs target measurement.

Raw JSON: [original](docs/benchmarks/2026-09-07-before.json), [final default rate](docs/benchmarks/2026-09-07-final.json), [final 10 MS/s](docs/benchmarks/2026-09-07-final-10msps.json). Final artifacts include the executable SHA-256. The earlier files named [after](docs/benchmarks/2026-09-07-after.json) and [after-10msps](docs/benchmarks/2026-09-07-after-10msps.json) record the intermediate DSP-only stage before idle backoff, not the final binary. That stage processed 24–27 MS/s at the default rate with 10–20% loss.

```sh
python3 scripts/benchmark_runtime.py build-gcc16/rfdet --rates 30720000 --bandwidth 30000000 --seconds 5 --repeat 2
python3 scripts/benchmark_runtime.py build-gcc16/rfdet --rates 10000000 --bandwidth 9000000 --seconds 5 --require-zero-loss
```

Remaining coverage opportunities from review: direct same-detector sample-rate/bandwidth cache-invalidation fixtures and isolated close-only output failure injection. Current implementation is correct by inspection and broader regression checks, but those individual boundaries are not directly tested. Real receiver behavior, Pi/NEON execution, sustained thermals, end-to-end alert latency and field classification remain unverified.

---

# Historical: initial native GCC 16 validation, before refactoring

Executed 2026-09-07 (America/Los_Angeles), Linux x86-64, Intel Core Ultra 9 185H, `/usr/bin/g++` 16.2.1 20260819 (Red Hat 16.2.1-2), CMake 4.3.0 / Ninja.

The current project has been moved from the nested `rf-detector/` directory to the repository root. The following checks use that final layout.

| Check | Result | Scope |
|---|---|---|
| `CXX=/usr/bin/g++ ./build.sh --out build-gcc16` | PASS | C++26, `-fmodules`, `--compile-std-module`, `import std`, optimized native build |
| Seven `build-gcc16/test_*` executables | PASS | acquire, dsp, fft, memory, pipeline, regression, simd; scalar x86-64 |
| `python3 scripts/verify_replay.py build-gcc16/rfdet` | PASS | 147,456 samples per read size, 10 identical event records, invalid inputs/output preservation |
| CMake Release build and CTest | PASS, 7/7 | GCC 16 C++23 modules with standard-header fallback |
| Native `rf.soapy` module plus `test_soapy` using `tests/soapy_stub` | PASS | Actual C++26/import-std module compilation and shim behavior, no SDK or receiver |
| `./build.sh --tsan --out build-gcc16-tsan` | LINK BLOCKED | Module compilation completed; linker cannot find `/usr/lib64/libtsan.so.2.0.0`; no native TSan execution claimed |
| `bash -n build.sh`, `git diff --check` | PASS | Shell syntax and tracked diff whitespace |

At this initial checkpoint, build warnings remained: misleading indentation in `tests/test_dsp.cpp:143`, and GCC's optimized `std::sort` array-bounds diagnostics for the four-reference array in the CMake Release build. Both were subsequently resolved by the refactor.

## Compiler workaround

The unmodified native build failed importing `rf.fft` into `rf.dsp` with `failed to read compiled module cluster ...: Bad file data`. A separate minimal named module reproduced it with just:

```cpp
export module lifetime;
import std;
export inline unsigned* establish(void* p, std::size_t n) {
    return std::start_lifetime_as_array<unsigned>(p, n);
}
```

Compiling a consumer that calls `establish` failed under the same compiler/options. Replacing that call with nonallocating placement array new, with `<new>` in the global module fragment, made the reproducer compile, link and run. `rf.memory` now selects its existing placement-new path for GCC 16 module builds. The `OverwriteRecord` constraint keeps default initialization trivial; no value initialization or per-slot clearing was added. Other compilers retain the feature-test selection. This remains a native GCC 16 `import std` build.

## Measured mock runs

These runs used the canonical `-O2` binary, no concurrent project build, default FFT/hop and no CSV sink. They are short host observations, not receiver or Pi benchmarks.

| Configuration | Received | Admitted = processed | Discarded | Continuity resets | Wall time including drain |
|---|---:|---:|---:|---:|---:|
| 30.72 MS/s, 30 MHz bandwidth, `--seconds 5 --no-lock` | 153,747,405 | 87,136,633 | 66,610,772 (43.32%) | 385 | 5.19 s |
| 10 MS/s, 9 MHz bandwidth, `--seconds 3 --no-lock` | 30,067,907 | 30,067,907 | 0 | 0 | 3.02 s |

Both runs returned every pool slot, had zero ownership violations, zero unknown hardware gaps, zero dropped event records and exited with status zero. Zero exit status does not imply zero sample loss. The default-rate run shows a capacity problem requiring measurement and tuning before live use.

The actual SoapySDR SDK/runtime is absent, so `--soapy` with the real SDK, physical receiver behavior, NEON, Pi thermals, antenna behavior and field detection accuracy remain unverified. ASan/UBSan were not rerun in this native validation session. The older validation record below is retained as historical evidence, not as checks rerun here.

---

# Historical: inherited validation record, option 2

Validation date: 2026-09-08. Host: Linux x86-64, GCC 13.3.0.

## Executed checks

| Check | Result | Scope |
|---|---|---|
| Seven portable test executables | PASS | acquire, dsp, fft, memory, pipeline, regression, simd |
| ASan + UBSan, same seven tests | PASS | LeakSanitizer disabled because this environment cannot inspect /proc under its tracing restrictions |
| ThreadSanitizer | PASS | memory's 300,000 cross-thread handoffs and pipeline drain/overload tests |
| CLI replay, release and ASan/UBSan host | PASS | 147,456 samples, 10 identical event records with read sizes 127 and 131072; zero discarded samples |
| CLI failure boundaries | PASS | malformed raw file, invalid FFT size, refusal to overwrite an existing CSV |
| Optional Soapy C API shim, release and ASan/UBSan | PASS | configuration/readback, scale, timestamps, errors, rollback, stop and cleanup |
| GCC 13 module compile-only diagnostic | PASS for non-Soapy modules, main and seven test translation units | O0 header path; not a supported native build or link/runtime validation |
| GCC 13 Soapy module attempt | Compiler internal error | unsupported compiler while reopening imported namespace |
| Shell build syntax | PASS | bash -n build.sh |

The portable scripts concatenate implementation bodies after removing module declarations/imports. They test algorithms and ownership, but cannot prove native module visibility or import-std correctness. Native GCC >=14 builds, GCC16 import std, CMake/Ninja and physical hardware were unavailable. Do not reuse the original README's GCC14, qemu or aarch64 claims as evidence for this revision.

## Regression coverage

- FFT impulse, signed-frequency tones, independent double-precision DFT and Parseval normalization, including N=8192.
- Continuous sample assembly across tiny/full reads; exact window/event positions, peak data and projected hardware time.
- Simultaneous positive/negative frequency tones; zero startup, clipping, a 20 dB broad noise step, explicit gaps, sustained begin/update/end events, selected ranges, narrow receive bandwidth and exact single-bin center.
- Duplicate pool return, use before initialization, repeat initialization preserving live contents, and failed memlock initialization returning locked-byte accounting to baseline.
- Deterministic overload: 32,768 samples received, 8,192 admitted and 24,576 discarded while DSP is delayed. Positive overflow payloads are included in known received/discarded totals.
- Fatal read failures, producer-done draining, all slots returned, and zero protocol violations.
- Actual CLI replay across fragmentation, event phase consistency, and output-file preservation.
- Soapy test headers are deliberately a minimal test-only API subset. They are never included by the hardware build.

## Review resolutions

Independent review findings resolved in this revision: direct module imports and standard-header dependencies; floor probes within usable spectrum; half-bin frequency bias; configurable detection masks; positive-overflow sample accounting; and macro headers required by import std. The event lifecycle was separately reviewed after begin/update/end records were introduced.

## Required target validation

1. Run the canonical build and all tests with the intended GCC version; repeat with the optional SDK/adapter.
2. Run scalar/NEON parity on the Pi. Only scalar execution was available here.
3. Verify real receiver configuration rounding, native full scale, timestamps, disconnect/overflow and bounded stop behavior.
4. Measure sustained sample admission, CPU load, temperatures and event latency at the selected sample rate. The default 30.72 MS/s is not a demonstrated throughput result.
5. Use labeled recordings to evaluate false alarms/misses and tune the spectral parameters. RF energy alone has no validated vehicle-classification accuracy.

Reproduction commands are in README.md. No field test, antenna compatibility test or hardware purchase/install was performed.

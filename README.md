# rfdet: streaming RF observations

Option 2 implements continuous FFT analysis, frequency-band events, raw SC16 replay, and an optional SoapySDR receiver. It retains the C++ module layout, fixed pools and SPSC queues.

This revision observes elevated RF energy. It does not identify police vehicles, decode radio protocols, or establish that a road is clear. The previous design's vehicle classification and fingerprinting claims are not implemented or validated.

## Build and run

The canonical build requires GCC >=14 and Linux/POSIX APIs. GCC 14 uses standard headers; GCC 15 builds the standard module from bits/std.cc; GCC 16 uses --compile-std-module. See [GCC's module changes](https://gcc.gnu.org/gcc-16/changes.html). The native GCC 16.2.1 C++26 `import std` build is supported and verified on Linux x86-64. GCC 16 module builds use placement array new for trivial pool storage to work around a reproducible compiler failure loading `std::start_lifetime_as_array` instantiations; see [VALIDATION.md](VALIDATION.md).

```sh
./build.sh
# This host: /usr/bin/g++ is GCC 16.2.1 (there is no g++-16 alias).
CXX=/usr/bin/g++ ./build.sh --out build-gcc16
./build/rfdet --seconds 3 --no-lock --events mock-events.csv

# Raw little-endian signed int16 I,Q, no header; runs to EOF.
./build/rfdet --input capture.sc16 --no-lock --events replay-events.csv

# Optional: install SoapySDR development files and the receiver's driver first.
./build.sh --soapy
./build/rfdet --device 'driver=bladerf' --seconds 10 --no-lock --events live-events.csv

# Raspberry Pi 5 target build:
CXX=g++ ./build.sh --march cortex-a76 --out build-pi
```

CSV output paths must be new files. Use --help for frequency, sample rate, bandwidth, gain, full scale, FFT size and replay read-size options. Defaults are 800 MHz center, 30.72 MS/s, 30 MHz usable bandwidth, 8192 FFT points, 4096-sample hops and a 12 dB local-background start threshold. These defaults are not a claim that a receiver or CPU sustains them.

Replay files have no metadata. Supply the actual capture values with --frequency, --sample-rate, --bandwidth, --gain and --full-scale. Default full scale is 32768. Soapy queries the driver's native CS16 full scale and reads actual settings back; other native formats are rejected.

Memory locking is enabled by default and can fail under a small memlock limit. Use --no-lock for development. A service can set LimitMEMLOCK=64M for the default topology; check startup when changing dimensions. CPU affinity is configurable through PipelineConfig.

Secondary CMake build, requiring CMake >=3.28, Ninja and a module-capable compiler:

```sh
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
# Add -DRF_WITH_SOAPY=ON to configure the adapter.
```

## Verification

The canonical build produces eleven test executables, plus test_soapy when enabled. CTest registers those eleven, four Python CLI checks, and a hardware-free Soapy C API shim check when Python is available (16 checks total). Run each test executable and require exit status zero. For the GCC 16 build above:

```sh
for test in build-gcc16/test_*; do "$test" || exit; done
python3 scripts/verify_replay.py build-gcc16/rfdet
python3 scripts/verify_runtime.py build-gcc16/rfdet
python3 scripts/verify_observability.py build-gcc16/rfdet
python3 scripts/verify_cli.py build-gcc16/rfdet
python3 scripts/verify_soapy_stub.py --native # GCC >=16, actual modules with test-only API shim
```

```sh
# GCC 13-compatible body checks, NOT native module builds:
python3 scripts/verify_portable.py
python3 scripts/verify_replay.py .verify-release/rfdet
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 scripts/verify_portable.py --sanitizer address
python3 scripts/verify_portable.py --sanitizer thread --only test_memory test_pipeline
python3 scripts/verify_soapy_stub.py
python3 scripts/verify_soapy_stub.py --sanitizer address
```

Portable scripts remove module declarations/imports while preserving implementation bodies. They do not validate module visibility, import std, NEON or hardware. See [VALIDATION.md](VALIDATION.md) for actual results and remaining checks.

To measure executable source-line coverage, use a fresh GCC build directory so old counters do not contaminate the report:

```sh
cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage
cmake --build build-coverage -j 4
ctest --test-dir build-coverage --output-on-failure
python3 scripts/report_coverage.py build-coverage --missing
```

The report merges executable lines across module/template instantiations and excludes standard-library and test code. It measures the configured build, so Soapy, NEON, uninstantiated functions, and individual branches sharing a source line are not represented by that percentage. The Soapy shim is tested separately. `--gcov` selects a gcov executable matching the compiler. Portable sanitizer builds accept `CXX` and `LDFLAGS` for toolchain-specific runtime libraries.

## Events and configuration

CSV phases are 0=begin, 1=update, 2=end. Begin is published on detection; updates follow every 128 supporting windows by default. End reasons, meaningful only for phase 2, are 0=quiet, 1=gap, 2=invalid_input, 3=finish.

Sample and shifted-bin intervals are half-open. Sample positions describe supporting STFT windows within a segment, not exact transmitter key-up/key-down times. Power/background are normalized digital band power, not calibrated dBm. excess_db is relative to neighboring spectrum, not calibrated radio SNR or class confidence. Hardware time is usable only when hw_time_valid is 1.

Adjacent fixed-width bands can emit separate IDs for one signal. DetectorConfig::ranges selects up to 16 shifted-bin masks. Intersecting bands are selected whole; background references still use all usable spectrum. Masks are currently a C++ configuration option.

Event overload can drop any record phase. Check dropped_events and tolerate incomplete lifecycles. CSV uses line buffering: each complete header/event row leaves the stdio buffer during its write. This makes records visible to file tailers without waiting for shutdown, but does not guarantee storage latency or disk durability. Write/close failures request stop and produce a nonzero exit. The final summary reports actual admitted/processed/discarded samples, unknown gaps, analysis windows and ownership violations.

## Live health and bounded mock runs

The CLI writes `health state=...` lines to stderr at startup, every 250 ms, and after drain. `--status-ms N` controls the interval; `--stall-ms N` controls the no-progress threshold (default 1000 ms). Both accept integer values from 10 to 60000. Capture and DSP freshness use host monotonic time; they are independent of signal strength and hardware timestamps.

- `starting` / `calibrating`: reception or calibration has not established tracking yet.
- `tracking`: the detector has recent processing and calibrated input. This is operational status, not a classification or an all-clear.
- `degraded`: loss, gaps, invalid windows or event drops occurred since the last report, or detector confidence is uncertain.
- `unavailable`: no successful positive capture arrived before the stall threshold, or admitted DSP work stopped progressing. Timeouts may recover without restarting.
- `stopping` / `stopped` / `failed`: lifecycle status; fatal errors take precedence.

Every report includes cumulative sample/window/drop counters, capture/DSP ages and interval fault flags. Live fields are approximate independent atomic snapshots; final counts after joins are exact. Fault flags clear after a report, while cumulative loss remains visible. Capture staleness is checked on each report. The DSP stall deadline starts when pending work is observed and restarts on observed progress, so idle periods do not count as stalls; fault reporting can take the stall threshold plus up to two report intervals and scheduler delay.

Paced mock reads honor the read timeout even below one sample per second; timed runs and SIGTERM do not wait for a full 131072-sample frame. Shutdown still requires real drivers and output callbacks to return. The receiver adapter's timeout behavior needs hardware validation.

## Measure processing capacity

Run without other builds or benchmarks competing for CPU:

```sh
python3 scripts/benchmark_runtime.py build-gcc16/rfdet --rates 30720000 --seconds 5 --repeat 2
python3 scripts/benchmark_runtime.py build-gcc16/rfdet --rates 10000000 --bandwidth 9000000 --seconds 5 --require-zero-loss
```

The JSON report includes the executable SHA-256, actual input and processing rates, sample loss, unknown gaps, event loss, CPU time and accounting consistency. `--require-zero-loss` fails for missing input, sample/event loss, unknown gaps, incomplete processing or fatal execution. A zero-loss result alone does not prove the requested rate was supplied: also inspect `received_vs_requested_samples` and measured rates. These are paced mock host measurements, not hardware benchmarks.

## Current review

[REVIEW.md](REVIEW.md) records fixed defects and remaining product gaps. In two five-second 30.72 MS/s host mock runs, the refactor improved processing from 18–21 to 28.5–28.8 MS/s including drain, reducing sample loss from 30–40% to 5.1–5.9%. A five-second 10 MS/s run processed all 50,174,345 received samples with zero loss. Default-rate capacity remains unresolved; these short measurements do not establish sustained receiver or Pi performance. See [VALIDATION.md](VALIDATION.md) for raw results and the [completed plan](docs/superpowers/plans/2026-09-07-runtime-refactor.md).

## Layout

src/rf/health.cppm interprets live telemetry; output.cppm owns CSV serialization; fft.cppm contains the precomputed FFT; dsp.cppm assembles continuous windows; acquire.cppm supplies mock/replay; soapy.cppm wraps the optional device API; pipeline.cppm owns the worker pipeline. [DESIGN.md](DESIGN.md) defines the contracts.

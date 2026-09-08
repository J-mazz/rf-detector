# Runtime reliability and throughput refactor

User authorization: produce a plan and then implement it in this working repository.

## Scope and approach

Retain the C++ modules, bounded SPSC pools, FFT normalization, replay semantics and CSV schema. Refactor the existing runtime to address the reproduced shutdown, event visibility, silent capture failure and processing-capacity findings. No new third-party library, receiver hardware, classifier, GUI, deployment or field-effectiveness claim is included.

Alternatives considered: replacing the FFT backend requires another dependency and target benchmarking; a new alert transport/UI would not address the underlying health and continuity defects. Choose targeted acquisition fixes, a separate health observer and CSV output owner, and semantics-preserving hot-loop optimization first.

## Contracts

- Native GCC 16 C++26 `import std` remains the primary verified build; keep the CMake header/module path and existing GCC compatibility logic.
- Capture/DSP storage remains allocated before workers start. Consumers drain admitted work on stop; source stop runs after acquisition joins, avoiding concurrent driver stop/read races.
- Paced mock reads honor their timeout: produce only samples available by a deadline within the timeout, or return timeout with zero samples. Never advance the sample clock on timeout. Very low rates must not force a one-sample sleep longer than the timeout. Nonpaced mock fragmentation and samples remain deterministic.
- CSV keeps the existing header and columns and refuses existing paths. Each newline record, including the header, is flushed through stdio during the write; write/flush/close errors yield a fatal exit. This removes userspace buffering latency, not arbitrary filesystem or callback stalls.
- Publish a health line to stderr at startup, every 250 ms by default, and after draining. `--status-ms` accepts integer 10..60000; `--stall-ms` accepts integer 10..60000 and defaults to 1000. Health monitoring is observational: timeouts can recover without restarting the process.
- A reusable control-plane monitor observes atomic timestamps/counters only. Status values are `starting`, `calibrating`, `tracking`, `degraded`, `unavailable`, `stopping`, `stopped`, `failed`. Report unavailable when capture stops delivering positive successful reads beyond the stall threshold, or admitted work has no DSP progress beyond it. Include separate flags/counters for dropped samples, unknown gaps, invalid input, dropped events, and calibration. Never infer quiet or clear road from tracking.
- Live DSP window/invalid/reset counters and calibration state must be observable while workers run. Snapshot fields are independently atomic and approximate during operation, exact after joins; do not use snapshot counts for shutdown decisions.
- Optimize without changing FFT normalization, event phases/IDs, sample coordinates or frequency mapping. Keep threshold comparisons correct near floating-point boundaries. Preserve recorded event values within existing test tolerances and exact replay-fragmentation equivalence.
- Measure identical old/new mock configurations sequentially with no concurrent build. A benchmark reports counts, loss fraction, admission/processing rates, elapsed time and exit status; optional zero-loss checks must fail when loss or unknown gaps occur. No hardware speed guarantee follows from a host benchmark.
- Measured implementation extension: idle worker backoff sleeps for 50 microseconds after 64 pauses to reduce repeated-yield CPU use. Queued work and completion draining retain the existing ownership protocol. This adds polling latency plus scheduler overhead and requires target latency measurement.

## Validation and product boundary

Use failing CLI regressions for low-rate timed/signal shutdown and live CSV visibility; deterministic unit tests for freshness, timeout/overflow, progress, health recovery and terminal states; existing DSP/FFT/replay fixtures for signal semantics. Native and CMake tests must pass. Exercise native Soapy with the test shim and run available sanitizers, reporting missing runtimes accurately.

Hardware coverage, antenna/downconversion choice, labeled signal recordings, false-alert/miss measurements and target thermal tests remain product gates documented in REVIEW.md. Software changes cannot close them.

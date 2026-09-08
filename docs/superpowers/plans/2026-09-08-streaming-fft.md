# Streaming FFT implementation plan

> For agentic workers: use executing-plans or subagent-driven-development task by task.

Goal: implement approved option 2 with replayable, gap-aware FFT detection.
Architecture: acquisition owns raw/discard storage, DSP owns persistent FFT and
window state, and sink consumes pooled begin/update/end event records. Startup owns allocation.
Tech stack: GCC C++ modules, POSIX memory and threads, Python verification driver.
Spec: docs/superpowers/specs/2026-09-08-streaming-fft.md

## Global constraints
- Canonical GCC >=14; GCC16 import std remains primary.
- No exceptions, RTTI, or allocation in DSP/acquisition loops.
- FFT size 8192 and hop 4096 by default; all dimensions validated.
- Preserve the uploaded originals; deliver updated archive and corresponding files.
- Sampling claims require real hardware; portable tests verify bodies only.

## Task 1: FFT primitive
Files: src/rf/fft.cppm, tests/test_fft.cpp.
API: rf::fft::Complex {float re,im}; Radix2FFT::initialize(size_t,
RegionConfig)->MemoryError; reset(); execute(Complex*) const; size() const.
initialize prepares bit-reversal and negative-angle twiddles in pinned regions.
execute is in-place, unnormalized, nonallocating; reset releases regions.
- [x] Test impulse, signed-frequency tones, small independent DFT, and Parseval.
- [x] Observe missing primitive failure; implement; run tests and sanitizers.

## Task 2: Resource and stream contracts
Files: memory.cppm, signal.cppm, runtime.cppm, acquire.cppm, simd.cppm,
tests/test_memory.cpp, tests/test_acquire.cpp.
- [x] Reproduce duplicate release and failed-initialization resource retention.
- [x] Reject pool use before initialization and repeated initialize; add rollback.
- [x] Add segment/sample metadata, validity flags, explicit scale and sample counters.
- [x] Add deterministic raw-SC16 replay and frame-size-independent mock generation.

## Task 3: Streaming detector and pipeline
Files: dsp.cppm, pipeline.cppm, tests/test_dsp.cpp, tests/test_pipeline.cpp.
API: StreamingDetector initialize/configure, feed(PlanarIQBlock, callback, context),
finish(callback, context). Callback accepts immutable SpectralCandidate lifecycle records
and source CaptureMetadata, copied into an event pool by the pipeline.
- [x] Test a known-frequency tone and exact sample-window behavior across tiny reads.
- [x] Test zero startup, gain/noise change, explicit gaps, simultaneous signals,
      malformed dimensions, sustained events and shutdown closure.
- [x] Implement persistent Hann STFT, guarded reference groups, health state and events.
- [x] Test deterministic overload using a finite source with DSP intentionally delayed.
- [x] Implement discard reads, sample accounting, initialization rollback and draining.

## Task 4: Host, build, verification and documentation
Files: main.cpp, build.sh, CMakeLists.txt, scripts/verify_portable.py, README.md,
DESIGN.md, VALIDATION.md.
- [x] Wire mock/replay CLI and event CSV output; invalid input returns nonzero.
- [x] Update dependency order and ensure unsupported compilers fail clearly.
- [x] Run all regression tests; ASan/UBSan; attempt TSan and native module build.
- [x] Review complete implementation and resolve substantive findings.
- [x] Record actual validation, deferred hardware checks and build instructions.
- [x] Save source archive and matching uploaded files with their original identities.

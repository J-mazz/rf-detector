# Streaming FFT detector, approved option 2

Implement a single-receiver, allocation-free processing path using C++ modules,
fixed pools, SPSC handles, 8192-point periodic Hann STFT, and 4096-sample hops.
Samples must be assembled continuously across reads and reset on explicit gaps,
configuration changes, or mismatched first-sample positions. Capture metadata
carries segment/configuration identifiers, sample position, timestamp validity,
known sample losses and gap flags. Sample scale is explicit.

Detection uses configurable frequency intervals, guarded local spectral
background references, finite warmup and explicit Uncalibrated/Tracking/Uncertain
states. Sustained narrowband signals must remain detectable; invalid zero or
clipped input cannot seed the background. Elevated fixed-width spectral bands produce begin/update/end
records with stable IDs, with start/end sample positions and peak excess over background.
No inference of vehicle identity is implemented by energy detection.

Acquisition owns a separate discard buffer. Live overload drains to it; replay
waits for capacity. Count samples actually discarded, and represent unknown
hardware loss separately. Shutdown drains admitted records; incomplete events
close explicitly. Initialization rolls back every region and rejects reuse.
Per-slot ownership checks reject duplicate returns while preserving SPSC roles.

The first FFT backend is a precomputed radix-2 implementation, with no external
FFT dependency. This provides a tested reference implementation; Pi throughput
and any faster backend require target measurement. Mock and raw SC16 replay are
runnable. An optional SoapySDR C adapter is included with reproducible API-shim tests;
hardware integration claims require physical receiver tests.

The canonical build requires GCC >=14 (GCC16 import std). An explicit portable
verification script strips only module preambles to validate implementation
bodies under GCC13, and must never be called a native module-build validation.

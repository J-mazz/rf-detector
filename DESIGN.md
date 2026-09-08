# Option 2: continuous spectral detector

Acquisition writes SC16 samples, DSP assembles continuous windows, and a separate sink consumes immutable pooled observation records. Project-owned capture/DSP storage and FFT tables are allocated before workers start. Drivers and the output runtime can allocate or copy internally.

## Scope

Mock and replay work without hardware. The optional SoapySDR C adapter configures RX channel 0, queries actual settings and native CS16 full scale, and defers stream setup/activation until start(). Caller-owned output buffers do not imply end-to-end zero-copy. Its boundary follows the [official SoapySDR C API](https://pothosware.github.io/SoapySDR/doxygen/latest/Device_8h.html).

There is no police classifier, protocol decoder, radar/laser receiver, GPS join, identity database, feature tensor producer or Orin service in this revision. tensor_valid_hops is always zero. These software tests do not establish a stock WRX antenna's suitability. Receiver coverage, antenna matching and in-car interference need separate measurements.

## Stream contract

- Capacity bounds a read; only returned, fully written I,Q pairs are valid. SC16 conversion uses explicit sample_scale. Raw files are little-endian and contain whole pairs.
- Metadata includes configuration, sequence, segment/config ID, segment-relative first sample, known lost samples and gap flags.
- Host monotonic/wall timestamps record read completion. Valid hardware time identifies the first sample. Event hardware time is projected from a segment's first valid anchor and sample rate; host time is never substituted for it.
- Discontinuities, configuration changes, unexpected sample positions or explicit gaps close events and reset STFT carry/calibration. The runnable pipeline holds configuration fixed; retune scheduling is absent.
- Live overload drains into a dedicated discard buffer. Replay waits for capacity. Only actual returned samples increase discard counts.
- Driver overflow/error loss is unknown and counted separately. Positive overflow payloads are conservatively discarded and counted by length. The next admitted frame starts a new segment with accumulated known loss and gap flags. Shutdown losses still appear in counters if no later frame exists.

After normal draining, samples_processed equals samples_admitted, and samples_received equals samples_admitted plus samples_discarded. Unknown missing hardware samples are excluded from numeric sample totals. Malformed counts and repeated failures stop the host with nonzero status.

## Analysis and confidence

Default periodic Hann STFT: N=8192, H=4096. A persistent circular buffer spans reads. First analysis follows N segment samples, then every H. Finish closes events and drops incomplete tails without zero padding.

The precomputed radix-2 FFT is forward, unnormalized, and nonallocating during execution. Shifted bin k represents (k-N/2)*Fs/N. Per-bin power is abs(FFT(window*x))^2 / (N*sum(window^2)), so total spectral power is window-weighted mean complex sample power. Four bins form a default band. Band frequency averages bin centers; bandwidth is bin count times Fs/N. Hann leakage and tiling limit resolution.

Power-of-two ring/shift indices use masks. Hann normalization is cached at initialization, and usable-band geometry/reference counts are rebuilt on segment configuration changes. A linear prefilter skips bands clearly below the relevant threshold; the original dB calculation and inclusive comparison remain in use near entry/release boundaries. Four-reference medians use bounded insertion sort.

Usable bands exclude edges, DC and analog-bandwidth limits. Background is the median of up to four guarded neighboring block means, computed with prefix sums. This spatial estimator preserves sustained narrowband contrast without freezing an outdated temporal floor. Wide signals occupying references can suppress detection; this is not a calibrated CFAR detector.

Eight valid windows warm calibration. Defaults use 12 dB start, 6 dB release and two unsupported windows to end. Zero/nonfinite/excessively clipped windows close events and mark confidence uncertain. Broad floor shifts over 10 dB restart warmup. Narrow bandwidths need enough usable reference bands. Internal health is uncalibrated/tracking/uncertain, not a probability.

## Events

Each active band has a stable ID with begin, periodic update and end records. Begin exposes sustained activity before it stops. Updates/end retain original start, latest supporting window end, and energy/background at peak excess. Adjacent bands are not merged.

End reason applies only to end phase: quiet, gap, invalid_input or finish. Sample positions include window support and overlap. Event publication time is separate from source metadata. Source hardware time stays unavailable for a segment whose initial anchor lacks it.

The sink borrows an immutable record only for its callback; copy needed fields before returning. Bounded pool/queue overload can lose any phase, and dropped-record counts are explicit. There is no serialized wire ABI for the in-memory structures.

## Ownership and shutdown

Default storage: 16 capture slots of 131072 complex samples, dedicated discard storage, two conversion slots with two float planes, and 64 event slots. Queues carry handles. Per-slot ownership rejects duplicate returns, but does not protect against generation-stale handles after slot reuse.

Startup validates dimensions/limits, establishes object lifetimes, and rolls back every region on failure. Repeated initialization is rejected without changing live contents. Reset/reclaim require quiescent workers; source configuration is immutable during a run.

Acquisition passes capture handles to DSP, which returns raw storage immediately after conversion. DSP borrows/returns scratch synchronously on one thread. DSP passes event handles to the sink. A producer retains a handle after failed enqueue, reusing it later. Main reclaims held handles after joins, preserving the single releasing-thread contract during operation.

Producer-done flags follow the final push. Consumers drain before publishing done. Stop ends acquisition while admitted work drains through DSP and sink. Main joins workers, stops the source and reclaims held slots. Sink errors request stop and cause nonzero status. Callbacks and device reads must return for shutdown to finish. Mock pacing caps the samples and sleep to the read deadline; a timeout consumes no samples. Source stop remains after acquisition joins to avoid concurrent driver read/stop calls.

Idle/capacity-wait backoff spins briefly, then requests a 50 microsecond sleep after 64 pauses. Queued work drains immediately when observed. This reduces idle CPU use at the cost of polling latency plus scheduler delay; it does not promise hard real-time delivery.

## Operational health and output

`rf.runtime::StageState` publishes successful capture and DSP progress times using host monotonic clocks. DSP publishes window/invalid/reset counters and detector health after each processed frame. `rf.health::Monitor` is owned by the control plane and reads only atomic state; it never calls observers on the live DSP object. Fields are approximate snapshots during execution, with final accounting after worker joins.

The CLI polls status every 250 ms (configurable) and reports capture or pending-DSP staleness after 1000 ms (configurable). Sample loss, unknown gaps, event loss and invalid windows produce interval degradation flags. Cumulative counters survive recovery. No-data staleness is recoverable; it does not automatically terminate a receiver. Terminal failure takes precedence over stopped. Tracking means operational detector health, not a police classification or road-clear decision.

`rf.output::CsvWriter` exclusively creates the CSV, checks the header, writes line-buffered complete records on the sink thread and checks close errors after joins. This bounds userspace buffering to a single record write, without claiming bounded filesystem, device-driver or arbitrary callback latency. Health reports use a separate stderr stream and remain observable when there are no RF candidates.

## Remaining measurements

The reference FFT has no demonstrated 30.72 MS/s Pi throughput. Run native target builds/tests, check scalar/NEON parity, then measure sustained admission, drop counts, CPU load, temperature and sink latency. Exercise real device disconnect, overflow and stop. Use labeled recordings to measure false alarms/misses before tuning thresholds or replacing the FFT backend. No field accuracy claim is made.

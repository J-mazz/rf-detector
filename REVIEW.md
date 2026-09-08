# Repository review — 2026-09-07

The project is a functioning spectral-observation prototype with native GCC 16 validation. It is not yet a validated speed-enforcement alert product. The fixed pools, explicit sample accounting, continuous STFT, replay fixtures and producer-done draining provide a useful foundation. No obvious ownership/publication defect was found in the reviewed queue/pool paths.

## Refactor resolutions — 2026-09-07

| Original finding | Current status | Evidence |
|---|---|---|
| Target receive/classification chain | Open | No receiver, antenna/downconversion design or labeled field evaluation was added. |
| Default-rate processing capacity | Mitigated; still open | DSP caching/prefiltering and idle-worker sleeps improve measured processing. Default-rate sample loss remains; see [current benchmarks](VALIDATION.md). |
| Buffered CSV visibility | Fixed | Complete rows are line-buffered; live visibility, exclusive creation and fatal write handling pass integration checks. Filesystem/callback latency remains external. |
| Silent unavailable reception | Fixed | Periodic atomic health snapshots expose capture/DSP staleness, loss, gaps and calibration. Recovery and terminal states have regression coverage. |
| Low-rate mock shutdown | Fixed | Deadline-limited reads pass timed and SIGTERM cases, including a sub-Hz source. |

The [implementation plan](docs/superpowers/plans/2026-09-07-runtime-refactor.md) records the delivered work. Independent acquisition, health, DSP/runtime and idle-backoff reviews found no remaining material correctness defects. Software verification does not establish detection effectiveness in the WRX.

## Original findings, ordered by impact (historical)

The observations and source line numbers below describe the pre-refactor code. Use the resolution table above for current status.

1. **Product blocker: the receive and classification chain for the intended target is undefined.** `src/main.cpp:66` defaults to an 800 MHz center with 30 MHz usable bandwidth; `DESIGN.md:9` explicitly excludes radar/laser reception and police classification. For comparison, police radar systems use microwave bands including 10.525, 24.150 and 34.700 GHz ([FCC technical discussion](https://docs.fcc.gov/public/attachments/DA-04-4062A1.pdf)); Stalker's current Patrol uses [K-band antennas](https://stalkerradar.com/police-radar/patrol/). The configured 800 MHz window cannot directly observe those emissions. A receiver/antenna/downconversion design matching the target signals must precede an effectiveness claim. If the intended proxy is communications activity, this code still supplies energy observations without evidence tying them to enforcement. There are no labeled field recordings or measured false-alert/miss rates in this repository.

2. **P1: the default input rate exceeds demonstrated processing capacity.** `src/rf/pipeline.cppm:65` drains into discard storage when capture slots run out. In an isolated five-second canonical-build mock run, 66,610,772 of 153,747,405 received samples were discarded (43.32%), with 385 continuity resets. Each reset clears STFT carry/calibration and closes active events, creating observation gaps. A separate 10 MS/s run lost no samples. These are host measurements, not a proven target operating envelope. Profile the FFT/detector and scheduling, then choose and validate a sustainable rate on the intended receiver/Pi. See [VALIDATION.md](VALIDATION.md) for exact counts and conditions.

3. **P1 for an alert consumer: CSV visibility has no latency bound.** `src/main.cpp:111` selects a 64 KiB output buffer, and `csv_event()` at line 50 never flushes it. A native two-second, 10 MS/s run had zero visible file bytes at 0.8 seconds and 12,881 bytes after exit. A sparse live stream can leave a begin event buffered indefinitely. This limitation is documented and reasonable for logging, but the current CLI needs a live notification sink or bounded flush interval before its output can drive timely alerts. Without `--events`, the sink only counts observations.

4. **P1 for operation: unavailable reception is indistinguishable from quiet reception to an external consumer.** `src/rf/pipeline.cppm:74` retries timeouts indefinitely, and the overflow branch at line 75 bypasses the consecutive-failure cutoff. There is no periodic health output or sink notification for these states; final counters appear only after exit. A stalled receiver can therefore remain running without communicating that detection is unavailable. Track time since the last usable samples and publish capture, overload and calibration health separately from RF events.

5. **P2: low-rate mock pacing defeats the requested run duration.** `src/rf/acquire.cppm:85` sleeps until a whole block's sample deadline and ignores the read timeout. `--sample-rate 1000 --bandwidth 1000 --seconds 0.1 --no-lock` did not exit within a two-second subprocess deadline. Its first 131,072-sample block implies roughly 131 seconds of pacing; main joins acquisition before stopping the source. Bound mock read sizes/waits by timeout or make pacing cancellation aware.

## Initial flattening/build changes delivered

- Moved the current nested project into the repository root, preserving its source, tests, scripts and design documents.
- Made the canonical build script executable and ignored generated build/module caches.
- Added the narrow GCC 16 module workaround for `std::start_lifetime_as_array` in `src/rf/memory.cppm`; C++26 and `import std` remain enabled.
- Updated build guidance and recorded fresh native validation, warnings and remaining limitations.

The runtime refactor resolves the software defects listed in the current status table; it does not implement a new receiver, alert UI or classifier. Before a WRX trial or product evaluation, establish the target RF chain and sustained admission, measure alert/health delivery, then evaluate labeled recordings and in-car interference. A successful software build alone provides no evidence of reduced tickets.

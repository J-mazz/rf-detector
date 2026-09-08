# rfdet — vehicular RF sensor for law-enforcement emitter fingerprints

Target: 2014 WRX. Compute: Raspberry Pi 5 (capture + DSP) and a Jetson Orin
(inference), Ethernet between them. Status: dataplane primitives (this
revision) are built and tested; sensors and fusion are specified below and
land in the order given in §6.

## 1. What is actually detectable, and with what

A police vehicle is a bundle of persistent RF identifiers plus a few class
signals. Sub-6 GHz SDR captures some of them; the rest are cheaper to take
from purpose-built receivers. Pretending an SDR does all of it is how a
project stalls.

| Emitter | Band | Sensor | Identifier persistence |
|---|---|---|---|
| Police radar (X 10.525, K 24.125–24.150, Ka 33.4–36.0 GHz) | out of SDR reach | Valentine One Gen2 over BLE (ESP protocol, published; KESPL/ESPLibrary implement it). Reports band, frequency (MHz), direction, strength | frequency offset is a per-unit signature |
| P25 mobile/portable transmit, 700 MHz narrowband | 799–805 MHz | SDR | class signal; control-channel metadata (unencrypted even when voice is encrypted) gives unit/talkgroup IDs later via OP25 |
| P25 mobile transmit, 800 MHz (NPSPAC + interleaved pool) | 806–815 MHz | SDR | class signal, shared with utilities/transit/B-ILT |
| FirstNet Band 14 uplink | 788–798 MHz | SDR | weak alone: AT&T consumer devices also use B14 |
| In-car router hotspots (Cradlepoint, Sierra AirLink) | 2.4/5 GHz | Wi-Fi adapter in monitor mode (external, e.g. MT7921U) | BSSID persistent, OUI identifies vendor |
| Body cams, Axon Signal triggers, radios, Ford SYNC | BLE 2.4 GHz | Pi 5 onboard BLE (BlueZ) or nRF52840 sniffer | OUI + stable MACs on most fleet gear; consumer gear randomizes |
| TPMS | 315 / 433.92 MHz | SDR (rtl_433-class decoders) | 28/32-bit sensor IDs, unencrypted, four per vehicle — a persistent vehicle fingerprint once labeled |

**SDR window decision.** One 30.72 MS/s capture centered at 800 MHz spans
784.6–815.4 MHz and covers Band 14 UL, 700 MHz P25 mobile Tx and 806–815
MHz mobile Tx simultaneously. That is the only window the SDR needs to sit
on continuously; TPMS is a periodic hop to 315/433 MHz. `main.cpp` already
carries this configuration.

**Receiver.** int16 (SC16) interleaved intake, ≥30.72 MS/s, USB 3, bus
powered: bladeRF 2.0 micro xA4 (47 MHz–6 GHz) is the fit; LimeSDR Mini 2.0
works to 3.5 GHz; USRP B210 is the reference-grade option from the CSS
project. HackRF is int8 and would need a second intake kernel. All go
through SoapySDR, whose `readStream` writes into caller-owned buffers —
the pinned capture slot — which is what `rf.acquire::SDRSource` mirrors.

**GPS.** Every event carries wall time; position comes from a USB GNSS via
gpsd and is joined at storage time. Location is the strongest prior the
Bayesian layer gets (fixed K-band door openers, known speed-trap geometry).

## 2. Evidence model

Same discipline as the CSS detector: honest likelihood ratios, corroboration
before alert, low base rate respected.

- **Strong**: K/Ka radar with credible frequency; P25 mobile Tx burst in
  799–805; BLE/Wi-Fi OUI match to LE-specific vendors; any identifier already
  in the learned fleet database.
- **Moderate**: 806–815 mobile Tx; Cradlepoint/Sierra BSSIDs (ambulances,
  utilities, transit carry them too); X band without corroboration.
- **Weak**: Band 14 uplink activity; unknown TPMS IDs; unknown BLE.

**Entity resolution turns weak class evidence into strong identity
evidence.** Identifiers observed together inside a time/RSSI window resolve
to one vehicle entity. A confirmation (radar hit + button press, or a visual
tag) labels the whole bundle, so the TPMS IDs and BSSID of that patrol car
become strong evidence on every later encounter, radar or not. That is the
active-learning loop from the original design with the annotator being the
driver, one button, in the moment.

`p(y|x) = Σ_k p(y|x,E_k) p(E_k|D_t)` survives unchanged: experts are
per-sensor classifiers, the regime posterior is the location/time prior.

**Legal envelope (Oregon).** Radar detectors are lawful in private
vehicles. Passive reception of non-encrypted radio, explicitly including
police and public-safety systems, is exempt under 18 U.S.C. §2511(2)(g).
Out of scope by design: decrypting P25 voice, any cellular content, any
transmission.

## 3. Dataplane invariants (enforced in code, tested)

1. Allocation only at startup; `PinnedRegion::map` is the only allocator.
   Pools are transactional: usable iff `initialize()` returned `none`.
2. Allocated capacity ≠ constructed objects ≠ valid samples. Storage is
   `mmap` + `mlock` (not `MAP_LOCKED`), lifetime established with
   `std::start_lifetime_as_array` (GCC 16), contents overwrite-only. Dataplane
   records have no default member initializers (`OverwriteRecord` concept).
3. `RLIMIT_MEMLOCK` is checked against the process-wide locked total before
   mapping; the pipeline needs 11.8 MB and fails at startup with
   `memlock_budget` on the default 8 MiB limit. `LimitMEMLOCK=64M` in the unit.
4. Every pool has exactly one acquiring thread and one releasing thread
   (`SPSCFreeList`: ring write, then release-store of the cursor). A stage
   that cannot hand a slot downstream **keeps it** and overwrites it next
   iteration. No stage releases what it acquired; held slots are reclaimed
   by the main thread after join.
5. Queues carry trivially-copyable handles ≤16 bytes only (`Handle`
   concept); they never construct, move, or destroy.
6. Shutdown is a chain of `*_done` flags release-stored after the producer's
   final push; consumers drain with `drain_step`. `empty()` is never a
   termination criterion. Test asserts `processed == captured` after stop.
7. Partial reads are honored: the device's return count is the only
   source of `valid_complex`.
8. Timestamps are `clock_gettime(CLOCK_MONOTONIC)` nanoseconds plus a
   `CLOCK_REALTIME` twin for GPS/log correlation.
9. Detection thresholds are relative to a tracked noise floor (asymmetric,
   frozen during bursts), never absolute energy.
10. Kernels are bitwise-identical across backends (`-ffp-contract=off`,
    unfused NEON, fixed 4-lane fold order). Verified NEON vs scalar under
    qemu for every tail length 0..63 and full frames.
11. Tensor layout is time-major `[hop][bin]`, model input `[1,1,Hops,Bins]`,
    symmetric int8 with explicit scale/zero-point; bin ranges are half-open.
12. No exceptions, no RTTI in the dataplane (`-fno-exceptions -fno-rtti`);
    failures are counters (`StageState`), and `protocol_violations` must stay 0.

## 4. Toolchain topology

- **Pi 5**: Fedora 44 aarch64 (Pi 5 images exist since March 2026; OS on
  microSD only, NVMe/thermal support were still in progress at release — put
  the event store on a USB SSD and verify thermal throttling before the car
  install). GCC 16.1.1 (16.2 arrives with Fedora 45). `build.sh` with
  `--march cortex-a76`. `import std;` via `--compile-std-module`.
- **Orin**: JetPack 7.2 = Ubuntu 24.04, CUDA 13.2, TensorRT 10.16, GCC 13.
  No C++26 modules there; the inference service is a separate build against
  a versioned wire layout of `CandidateEvent`. Nothing in `rf.*` depends on
  it. If the encoder stays small, ONNX Runtime on the Pi's CPU is a viable
  first stop and the Orin becomes an optimization.
- **Dev/CI**: GCC 14/15 build the identical sources through the header
  fallback (`RF_IMPORT_STD` undefined); ThreadSanitizer on x86-64; NEON
  parity on aarch64 via qemu-user.
- `std::simd` (C++26, experimental in libstdc++ 16) is the candidate for the
  portable middle tier between `GenericFallback` and `NeonBackend`; NEON
  stays for the structure loads (`vld2`) it cannot express.

## 5. Module graph (this revision)

```
rf.core      handles, clocks, OverwriteRecord/Handle concepts, error enum
rf.memory    PinnedRegion, SPSCFreeList, FixedPool, ObjectPool, memlock budget
rf.runtime   HandleQueue, StageState/Counter, drain_step, cpu_relax, pinning
rf.signal    CaptureMetadata, Raw/PlanarIQBlock, SpectralCandidate, EventTensor, CandidateEvent
rf.simd      GenericFallback → NeonBackend → DefaultSIMDBackend
rf.dsp       NoiseTracker, EnergyDetector<Backend>
rf.acquire   SDRSource (SoapySDR-shaped), MockSource
rf.pipeline  acquisition → dsp → sink, retained-slot backpressure, drain shutdown
```

## 6. Next revisions, in order

1. `rf.acquire::SoapySource` (bladeRF/Lime/USRP), CS16, direct-to-slot reads.
2. `rf.dsp` channelizer: STFT over the 785–815 window, per-channel energy
   with the same tracker, emits `SpectralCandidate` with real bin ranges and
   fills `EventTensor`; TPMS hop schedule.
3. `rf.store`: append-only DuckDB tables (`observations`, `entities`,
   `identifiers`, `labels`, `alerts`), GPS join, retention policy from the
   original design (metadata → features → tensor → raw IQ).
4. Sensor adapters: V1 Gen2 ESP/BLE, BlueZ BLE scanner, Wi-Fi monitor
   capture, gpsd. Each emits into the same event stream via `EventSink`.
5. Entity resolution + Bayesian fusion (`rf.probability`), the one-button
   labeler, then the encoder/MoE on the Orin.

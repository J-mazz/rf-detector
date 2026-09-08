// rf.pipeline — stage topology for one receiver.
//
//   acquisition ──HandleQueue──▶ dsp ──HandleQueue──▶ sink
//        │ acquires capture slots        │ acquires event slots      │ releases event slots
//        │ (sole acquirer)               │ releases capture slots    │ (sole releaser)
//        │                               │ (sole releaser)           │
//
// Backpressure rule: a stage that cannot hand a slot downstream KEEPS it
// and overwrites it on the next iteration (drop-newest). No stage ever
// releases a slot it acquired, so every pool keeps exactly one acquirer
// and one releaser. Held slots are reclaimed by the main thread after join.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <atomic>
#include <cstddef>
#include <cstdint>
#endif
#include <sched.h>

export module rf.pipeline;

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.memory;
import rf.runtime;
import rf.signal;
import rf.simd;
import rf.dsp;
import rf.acquire;

export namespace rf::pipeline {

using rf::core::InvalidSlot;
using rf::core::SlotIndex;

struct Topology {
    static constexpr std::size_t FrameComplex          = 131'072;   // 6.55 ms at 20 MS/s
    static constexpr std::size_t CapturePoolDepth      = 16;        // 8 MiB int16 IQ, ~105 ms of headroom
    static constexpr std::size_t ScratchDepth          = 2;         // acquire/release on the same thread
    static constexpr std::size_t ScratchPlanes         = 3;         // I, Q, |x|^2
    static constexpr std::size_t EventPoolDepth        = 64;
    static constexpr std::size_t CaptureQueueDepth     = 16;
    static constexpr std::size_t EventQueueDepth       = 64;
    static constexpr std::size_t MaxCandidatesPerFrame = 128;
};

struct PipelineConfig {
    int           acquisition_cpu;   // -1: unpinned
    int           dsp_cpu;
    int           sink_cpu;
    std::uint32_t read_timeout_us;
    std::uint32_t max_consecutive_failures;
    rf::dsp::EnergyDetectorConfig detector;
};
inline constexpr PipelineConfig DefaultPipelineConfig{-1, -1, -1, 100'000, 8, rf::dsp::DefaultEnergyDetector};

// Cheap wait: spin on an isolated core, yield elsewhere.
struct Backoff {
    std::uint32_t spins{0};
    void pause() noexcept {
        if (++spins < 64) { rf::runtime::cpu_relax(); return; }
        spins = 0;
        (void)::sched_yield();
    }
};

class PipelineSystem {
public:
    using CapturePool = rf::memory::FixedPool<std::int16_t, 2 * Topology::FrameComplex, Topology::CapturePoolDepth>;
    using ScratchPool = rf::memory::FixedPool<float, Topology::ScratchPlanes * Topology::FrameComplex, Topology::ScratchDepth>;
    using EventPool   = rf::memory::ObjectPool<rf::signal::CandidateEvent, Topology::EventPoolDepth>;
    using CaptureQueue = rf::runtime::HandleQueue<SlotIndex, Topology::CaptureQueueDepth>;
    using EventQueue   = rf::runtime::HandleQueue<SlotIndex, Topology::EventQueueDepth>;
    using Backend      = rf::simd::DefaultSIMDBackend;

    PipelineSystem(rf::acquire::SDRSource& source, rf::runtime::StageState& state,
                   PipelineConfig cfg = DefaultPipelineConfig) noexcept
        : source_(source), state_(state), cfg_(cfg), detector_(cfg.detector) {}

    PipelineSystem(const PipelineSystem&) = delete;
    PipelineSystem& operator=(const PipelineSystem&) = delete;

    // Transactional: either every pool is mapped (and locked, if requested)
    // or nothing is and the error names the first failure.
    [[nodiscard]] rf::memory::MemoryError initialize(
        rf::memory::RegionConfig region = rf::memory::DefaultRegionConfig) noexcept
    {
        using rf::memory::MemoryError;
        if (auto e = capture_pool_.initialize(region); e != MemoryError::none) return e;
        if (auto e = scratch_pool_.initialize(region); e != MemoryError::none) return e;
        if (auto e = event_pool_.initialize(region);   e != MemoryError::none) return e;
        ready_ = true;
        return MemoryError::none;
    }

    static constexpr std::size_t locked_bytes_required() noexcept {
        return CapturePool::payload_bytes() + ScratchPool::payload_bytes() + EventPool::payload_bytes();
    }

    // ---- stage bodies (each runs on its own thread) ------------------------

    void run_acquisition() noexcept {
        if (cfg_.acquisition_cpu >= 0) (void)rf::runtime::pin_current_thread(cfg_.acquisition_cpu);
        const rf::acquire::ReceiverConfig& rc = source_.config();
        Backoff backoff;
        std::uint32_t failures = 0;
        std::uint32_t dropped_since_delivery = 0;

        while (!state_.stopping()) {
            if (held_capture_ == InvalidSlot) {
                held_capture_ = capture_pool_.try_acquire();
                if (held_capture_ == InvalidSlot) {
                    state_.capture_pool_exhausted.add();
                    ++dropped_since_delivery;
                    backoff.pause();
                    continue;
                }
            }

            std::int16_t* dst = capture_pool_.slot(held_capture_);
            const rf::acquire::ReadResult r =
                source_.read_into(dst, Topology::FrameComplex, cfg_.read_timeout_us);

            switch (r.status) {
                case rf::acquire::ReadStatus::ok: failures = 0; break;
                case rf::acquire::ReadStatus::timeout:  state_.rx_timeouts.add(); continue;
                case rf::acquire::ReadStatus::overflow: state_.rx_overruns.add(); ++dropped_since_delivery; continue;
                case rf::acquire::ReadStatus::failure:
                    state_.rx_failures.add();
                    if (++failures >= cfg_.max_consecutive_failures) state_.request_stop();
                    continue;
                case rf::acquire::ReadStatus::stopped: state_.request_stop(); continue;
            }
            if (r.complex_samples == 0) continue;
            if (r.complex_samples < Topology::FrameComplex) state_.frames_partial.add();

            SlotHeader& h = headers_[held_capture_];
            h.valid_complex             = r.complex_samples;
            h.meta.timestamp_ns         = rf::core::monotonic_now_ns();
            h.meta.wall_ns              = rf::core::wall_now_ns();
            h.meta.hw_time_ns           = r.hw_time_ns;
            h.meta.center_frequency_hz  = rc.center_frequency_hz;
            h.meta.sample_rate_sps      = rc.sample_rate_sps;
            h.meta.gain_db              = rc.gain_db;
            h.meta.receiver_id          = rc.receiver_id;
            h.meta.sequence             = sequence_++;
            h.meta.dropped_frames_prior = dropped_since_delivery;
            h.meta.reserved0            = 0;

            if (!capture_queue_.try_push(held_capture_)) {
                state_.dsp_backpressure.add();      // retain: next read overwrites this slot
                ++dropped_since_delivery;
                continue;
            }
            state_.frames_captured.add();
            dropped_since_delivery = 0;
            held_capture_ = InvalidSlot;
        }
        state_.acquisition_done.store(true, std::memory_order_release);
    }

    void run_dsp() noexcept {
        if (cfg_.dsp_cpu >= 0) (void)rf::runtime::pin_current_thread(cfg_.dsp_cpu);
        Backoff backoff;
        SlotIndex raw = InvalidSlot;

        for (;;) {
            const rf::runtime::Drain d =
                rf::runtime::drain_step(capture_queue_, state_.acquisition_done, raw);
            if (d == rf::runtime::Drain::finished) break;
            if (d == rf::runtime::Drain::idle) { backoff.pause(); continue; }

            const SlotHeader& h = headers_[raw];
            const SlotIndex scratch = scratch_pool_.try_acquire();
            if (scratch == InvalidSlot) {
                state_.scratch_exhausted.add();
                if (!capture_pool_.release(raw)) state_.protocol_violations.add();
                continue;
            }

            float* planes = scratch_pool_.slot(scratch);
            rf::signal::PlanarIQBlock block;
            block.i                = planes;
            block.q                = planes + Topology::FrameComplex;
            block.capacity_complex = Topology::FrameComplex;
            block.count_complex    = h.valid_complex;                 // honors partial reads
            block.meta             = h.meta;
            float* magsq = planes + 2 * Topology::FrameComplex;

            Backend::deinterleave_and_convert_s16_f32(
                capture_pool_.slot(raw), block.i, block.q, block.count_complex);

            // Raw slot goes back as early as possible: capture headroom > DSP latency.
            if (!capture_pool_.release(raw)) state_.protocol_violations.add();

            const std::size_t n = detector_.scan(block, magsq, candidates_,
                                                 Topology::MaxCandidatesPerFrame,
                                                 rf::core::monotonic_now_ns());
            state_.candidates.add(n);
            for (std::size_t k = 0; k < n; ++k) emit_event(candidates_[k], block.meta);

            if (!scratch_pool_.release(scratch)) state_.protocol_violations.add();
            state_.frames_processed.add();
        }
        state_.dsp_done.store(true, std::memory_order_release);
    }

    // Local sink: consumes events and returns their slots. The Orin transport
    // (rf.transport) will replace the body of this loop, not its structure.
    void run_sink() noexcept {
        if (cfg_.sink_cpu >= 0) (void)rf::runtime::pin_current_thread(cfg_.sink_cpu);
        Backoff backoff;
        SlotIndex ev = InvalidSlot;
        for (;;) {
            const rf::runtime::Drain d =
                rf::runtime::drain_step(event_queue_, state_.dsp_done, ev);
            if (d == rf::runtime::Drain::finished) break;
            if (d == rf::runtime::Drain::idle) { backoff.pause(); continue; }

            const rf::signal::CandidateEvent* e = event_pool_.slot(ev);
            last_snr_db_.store(e->detection.snr_db, std::memory_order_relaxed);
            state_.events_consumed.add();
            if (!event_pool_.release(ev)) state_.protocol_violations.add();
        }
        state_.sink_done.store(true, std::memory_order_release);
    }

    // Main thread, after every stage thread has been joined.
    void reclaim_held_slots() noexcept {
        if (held_capture_ != InvalidSlot) {
            if (!capture_pool_.release(held_capture_)) state_.protocol_violations.add();
            held_capture_ = InvalidSlot;
        }
        if (held_event_ != InvalidSlot) {
            if (!event_pool_.release(held_event_)) state_.protocol_violations.add();
            held_event_ = InvalidSlot;
        }
    }

    // ---- observers ---------------------------------------------------------
    [[nodiscard]] std::size_t capture_slots_free() const noexcept { return capture_pool_.approx_free(); }
    [[nodiscard]] std::size_t scratch_slots_free() const noexcept { return scratch_pool_.approx_free(); }
    [[nodiscard]] std::size_t event_slots_free()   const noexcept { return event_pool_.approx_free(); }
    [[nodiscard]] float last_snr_db() const noexcept { return last_snr_db_.load(std::memory_order_relaxed); }
    // DSP-thread state: read only after the DSP thread has been joined.
    [[nodiscard]] float noise_floor_db() const noexcept { return detector_.noise_floor_db(); }
    [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
    struct alignas(rf::core::CacheLineSize) SlotHeader {   // one line per slot: no false sharing
        rf::signal::CaptureMetadata meta;
        std::size_t valid_complex;
    };

    void emit_event(const rf::signal::SpectralCandidate& cand,
                    const rf::signal::CaptureMetadata& meta) noexcept
    {
        if (held_event_ == InvalidSlot) {
            held_event_ = event_pool_.try_acquire();
            if (held_event_ == InvalidSlot) { state_.event_pool_exhausted.add(); return; }
        }
        rf::signal::CandidateEvent* e = event_pool_.slot(held_event_);
        e->event_id          = next_event_id_++;
        e->timestamp_ns      = cand.detected_at_ns;
        e->wall_ns           = meta.wall_ns;
        e->source_meta       = meta;
        e->detection         = cand;
        e->quant             = rf::signal::TensorQuant{0.0f, 0, {0, 0, 0}};
        e->tensor_valid_hops = 0;                       // STFT tensor: not produced in v0
        // e->tensor intentionally untouched: overwrite-only storage.

        if (!event_queue_.try_push(held_event_)) {
            state_.event_backpressure.add();            // retain: next candidate overwrites
            return;
        }
        state_.events_emitted.add();
        held_event_ = InvalidSlot;
    }

    rf::acquire::SDRSource&    source_;
    rf::runtime::StageState&   state_;
    PipelineConfig             cfg_;
    rf::dsp::EnergyDetector<Backend> detector_;

    CapturePool  capture_pool_;
    ScratchPool  scratch_pool_;
    EventPool    event_pool_;
    CaptureQueue capture_queue_;
    EventQueue   event_queue_;

    alignas(rf::core::CacheLineSize) SlotHeader headers_[Topology::CapturePoolDepth];
    rf::signal::SpectralCandidate candidates_[Topology::MaxCandidatesPerFrame];   // DSP thread only

    SlotIndex     held_capture_{InvalidSlot};   // acquisition thread only
    SlotIndex     held_event_{InvalidSlot};     // dsp thread only
    std::uint64_t sequence_{0};
    std::uint64_t next_event_id_{1};
    std::atomic<float> last_snr_db_{0.0f};
    bool          ready_{false};
};

} // namespace rf::pipeline

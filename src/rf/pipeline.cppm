// Single receiver: acquisition -> DSP -> event sink. All resource setup is serial.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <atomic>
#include <cmath>
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
using rf::core::InvalidSlot;using rf::core::SlotIndex;
struct Topology {
    static constexpr std::size_t FrameComplex=131072,CapturePoolDepth=16,CaptureQueueDepth=16;
    static constexpr std::size_t ScratchDepth=2,ScratchPlanes=2,EventPoolDepth=64,EventQueueDepth=64;
};
using SinkCallback=void(*)(const rf::signal::CandidateEvent&,void*) noexcept;
struct PipelineConfig {
    int acquisition_cpu=-1,dsp_cpu=-1,sink_cpu=-1;
    std::uint32_t read_timeout_us=100000,max_consecutive_failures=8;
    rf::dsp::DetectorConfig detector{};
    SinkCallback sink=nullptr;void* sink_context=nullptr;
};
inline constexpr PipelineConfig DefaultPipelineConfig{};
struct Backoff {
    std::uint32_t spins=0;
    void pause() noexcept {
        if(++spins<64){rf::runtime::cpu_relax();return;}
        spins=0;
        // Empty queues should not consume a full CPU through repeated yield().
        // The short sleep trades at most one polling interval (plus scheduler
        // delay) for lower idle power; queued work still drains without sleeps.
        rf::core::sleep_ns(50'000);
    }
};
class PipelineSystem {
public:
    using CapturePool=rf::memory::FixedPool<std::int16_t,2*Topology::FrameComplex,Topology::CapturePoolDepth>;
    using ScratchPool=rf::memory::FixedPool<float,2*Topology::FrameComplex,Topology::ScratchDepth>;
    using EventPool=rf::memory::ObjectPool<rf::signal::CandidateEvent,Topology::EventPoolDepth>;
    using Backend=rf::simd::DefaultSIMDBackend;
    PipelineSystem(rf::acquire::SDRSource& source,rf::runtime::StageState& state,PipelineConfig cfg={}) noexcept
        :source_(source),state_(state),cfg_(cfg){}
    PipelineSystem(const PipelineSystem&)=delete;PipelineSystem& operator=(const PipelineSystem&)=delete;
    [[nodiscard]] rf::memory::MemoryError initialize(rf::memory::RegionConfig region=rf::memory::DefaultRegionConfig) noexcept {
        using E=rf::memory::MemoryError;
        if(ready_)return E::already_initialized;
        if(!rf::acquire::valid_config(source_.config()) || !cfg_.read_timeout_us || !cfg_.max_consecutive_failures)return E::invalid_size;
        auto fail=[&](E e){capture_.reset_quiescent();scratch_.reset_quiescent();events_.reset_quiescent();discard_.unmap();detector_.reset();return e;};
        if(auto e=capture_.initialize(region);e!=E::none)return fail(e);
        if(auto e=scratch_.initialize(region);e!=E::none)return fail(e);
        if(auto e=events_.initialize(region);e!=E::none)return fail(e);
        if(auto e=discard_.map(2*Topology::FrameComplex,region);e!=E::none)return fail(e);
        if(auto e=detector_.initialize(cfg_.detector,region);e!=E::none)return fail(e);
        ready_=true;return E::none;
    }
    void run_acquisition() noexcept {
        if(!ready_){fatal();state_.acquisition_done.store(true,std::memory_order_release);return;}
        if(cfg_.acquisition_cpu>=0)(void)rf::runtime::pin_current_thread(cfg_.acquisition_cpu);
        const auto rc=source_.config();Backoff backoff;std::uint32_t failures=0;
        while(!state_.stopping()){
            if(held_capture_==InvalidSlot)held_capture_=capture_.try_acquire();
            const bool dropping=held_capture_==InvalidSlot;
            if(dropping){state_.capture_pool_exhausted.add();if(source_.lossless()){backoff.pause();continue;}}
            auto* dst=dropping?discard_.data():capture_.slot(held_capture_);
            const auto r=source_.read_into(dst,Topology::FrameComplex,cfg_.read_timeout_us);
            if(r.complex_samples>Topology::FrameComplex){state_.malformed_reads.add();fatal();break;}
            if(r.status==rf::acquire::ReadStatus::stopped){state_.request_stop();break;}
            if(r.status==rf::acquire::ReadStatus::timeout){state_.rx_timeouts.add();backoff.pause();continue;}
            if(r.status==rf::acquire::ReadStatus::overflow){
                // END_ABRUPT may carry valid samples. Conservatively discard them,
                // accounting for their known length separately from unknown loss.
                state_.samples_received.add(r.complex_samples);state_.samples_discarded.add(r.complex_samples);
                pending_lost_+=r.complex_samples;
                state_.rx_overruns.add();state_.unknown_gap_events.add();mark_gap(rf::signal::GapDevice);have_hw_=false;continue;
            }
            if(r.status==rf::acquire::ReadStatus::failure){
                state_.rx_failures.add();state_.unknown_gap_events.add();mark_gap(rf::signal::GapDevice);have_hw_=false;
                if(++failures>=cfg_.max_consecutive_failures)fatal();
                backoff.pause();continue;
            }
            if(!r.complex_samples){backoff.pause();continue;}
            failures=0;state_.samples_received.add(r.complex_samples);
            state_.last_capture_ns.store(rf::core::monotonic_now_ns(),std::memory_order_release);
            if(r.complex_samples<Topology::FrameComplex)state_.frames_partial.add();
            if(r.hw_time_valid && have_hw_){
                const long double expected=static_cast<long double>(last_hw_)+static_cast<long double>(last_hw_count_)*1e9L/rc.sample_rate_sps;
                if(std::fabs(static_cast<long double>(r.hw_time_ns)-expected)>std::max(2.0L,0.25e9L/rc.sample_rate_sps)){
                    state_.unknown_gap_events.add();mark_gap(rf::signal::GapDevice);
                }
            }
            have_hw_=r.hw_time_valid;last_hw_=r.hw_time_ns;last_hw_count_=r.complex_samples;
            if(dropping){state_.samples_discarded.add(r.complex_samples);pending_lost_+=r.complex_samples;mark_gap(rf::signal::GapDiscard);continue;}
            auto& h=headers_[held_capture_];h.count=r.complex_samples;h.meta=rf::signal::CaptureMetadata{};
            h.meta.timestamp_ns=rf::core::monotonic_now_ns();h.meta.wall_ns=rf::core::wall_now_ns();
            h.meta.hw_time_ns=r.hw_time_ns;h.meta.hw_time_valid=r.hw_time_valid;
            h.meta.center_frequency_hz=rc.center_frequency_hz;h.meta.sample_rate_sps=rc.sample_rate_sps;
            h.meta.bandwidth_hz=rc.bandwidth_hz;h.meta.gain_db=rc.gain_db;h.meta.receiver_id=rc.receiver_id;
            h.meta.sequence=sequence_++;h.meta.segment_id=segment_;h.meta.config_id=rc.config_id;
            h.meta.first_sample=sample_position_;h.meta.known_lost_samples=pending_lost_;h.meta.gap_flags=pending_gap_;
            if(!raw_queue_.try_push(held_capture_)){
                state_.dsp_backpressure.add();state_.samples_discarded.add(r.complex_samples);
                pending_lost_+=r.complex_samples;mark_gap(rf::signal::GapDiscard);continue;
            }
            state_.frames_captured.add();state_.samples_admitted.add(r.complex_samples);
            sample_position_+=r.complex_samples;pending_lost_=0;pending_gap_=0;held_capture_=InvalidSlot;
        }
        state_.acquisition_done.store(true,std::memory_order_release);
    }
    void run_dsp() noexcept {
        if(!ready_){fatal();state_.dsp_done.store(true,std::memory_order_release);return;}
        if(cfg_.dsp_cpu>=0)(void)rf::runtime::pin_current_thread(cfg_.dsp_cpu);
        Backoff backoff;SlotIndex raw=InvalidSlot;
        for(;;){
            const auto d=rf::runtime::drain_step(raw_queue_,state_.acquisition_done,raw);
            if(d==rf::runtime::Drain::finished)break;
            if(d==rf::runtime::Drain::idle){backoff.pause();continue;}
            const auto header=headers_[raw];const auto slot=scratch_.try_acquire();
            if(slot==InvalidSlot){state_.scratch_exhausted.add();fatal();if(!capture_.release(raw))state_.protocol_violations.add();continue;}
            auto* planes=scratch_.slot(slot);
            Backend::deinterleave_and_convert_s16_f32(capture_.slot(raw),planes,planes+Topology::FrameComplex,header.count,source_.config().sample_scale);
            if(!capture_.release(raw))state_.protocol_violations.add();
            rf::signal::PlanarIQBlock b{planes,planes+Topology::FrameComplex,Topology::FrameComplex,header.count,header.meta};
            if(!detector_.feed(b,on_candidate,this))fatal();
            if(!scratch_.release(slot))state_.protocol_violations.add();
            state_.samples_processed.add(header.count);state_.frames_processed.add();
            publish_analysis_health();
            state_.last_dsp_ns.store(rf::core::monotonic_now_ns(),std::memory_order_release);
        }
        detector_.finish(on_candidate,this);
        publish_analysis_health();
        state_.dsp_done.store(true,std::memory_order_release);
    }
    void run_sink() noexcept {
        if(!ready_){fatal();state_.sink_done.store(true,std::memory_order_release);return;}
        if(cfg_.sink_cpu>=0)(void)rf::runtime::pin_current_thread(cfg_.sink_cpu);
        Backoff backoff;SlotIndex ev=InvalidSlot;
        for(;;){
            const auto d=rf::runtime::drain_step(event_queue_,state_.dsp_done,ev);
            if(d==rf::runtime::Drain::finished)break;
            if(d==rf::runtime::Drain::idle){backoff.pause();continue;}
            const auto* e=events_.slot(ev);last_excess_.store(e->detection.excess_over_background_db,std::memory_order_relaxed);
            if(cfg_.sink)cfg_.sink(*e,cfg_.sink_context);
            state_.events_consumed.add();if(!events_.release(ev))state_.protocol_violations.add();
        }
        state_.sink_done.store(true,std::memory_order_release);
    }
    // Main thread only, after every worker joins. Source must also be stopped.
    void reclaim_held_slots() noexcept {
        if(held_capture_!=InvalidSlot){if(!capture_.release(held_capture_))state_.protocol_violations.add();held_capture_=InvalidSlot;}
        if(held_event_!=InvalidSlot){if(!events_.release(held_event_))state_.protocol_violations.add();held_event_=InvalidSlot;}
    }
    [[nodiscard]] bool ready() const noexcept{return ready_;}
    [[nodiscard]] std::size_t capture_slots_free() const noexcept{return capture_.approx_free();}
    [[nodiscard]] std::size_t scratch_slots_free() const noexcept{return scratch_.approx_free();}
    [[nodiscard]] std::size_t event_slots_free() const noexcept{return events_.approx_free();}
    [[nodiscard]] float last_excess_db() const noexcept{return last_excess_.load(std::memory_order_relaxed);}
    // DSP-owned observers: after DSP joins only.
    [[nodiscard]] float noise_floor_db() const noexcept{return detector_.noise_floor_db();}
private:
    void publish_analysis_health() noexcept {
        state_.analysis_windows.set(detector_.windows_processed());
        state_.invalid_windows.set(detector_.invalid_windows());
        state_.continuity_resets.set(detector_.continuity_resets());
        state_.detector_health.store(detector_.health(),std::memory_order_release);
    }
    void fatal() noexcept{state_.fatal_error.store(true,std::memory_order_relaxed);state_.request_stop();}
    void mark_gap(std::uint32_t flag) noexcept {if(!pending_gap_){++segment_;sample_position_=0;}pending_gap_|=flag;}
    static void on_candidate(const rf::signal::SpectralCandidate& c,const rf::signal::CaptureMetadata& m,void* ctx) noexcept {
        static_cast<PipelineSystem*>(ctx)->emit(c,m);
    }
    void emit(const rf::signal::SpectralCandidate& c,const rf::signal::CaptureMetadata& m) noexcept {
        state_.candidates.add();
        if(held_event_==InvalidSlot)held_event_=events_.try_acquire();
        if(held_event_==InvalidSlot){state_.event_pool_exhausted.add();state_.events_dropped.add();return;}
        auto* e=events_.slot(held_event_);e->event_id=c.event_id;e->timestamp_ns=rf::core::monotonic_now_ns();e->wall_ns=rf::core::wall_now_ns();
        e->source_meta=m;e->detection=c;e->quant=rf::signal::TensorQuant{0,0,{0,0,0}};e->tensor_valid_hops=0;
        // No tensor is claimed valid until a feature extractor writes it.
        if(!event_queue_.try_push(held_event_)){state_.event_backpressure.add();state_.events_dropped.add();return;}
        state_.events_emitted.add();held_event_=InvalidSlot;
    }
    struct alignas(rf::core::CacheLineSize) Header{rf::signal::CaptureMetadata meta;std::size_t count;};
    rf::acquire::SDRSource& source_;rf::runtime::StageState& state_;PipelineConfig cfg_;rf::dsp::StreamingDetector detector_;
    CapturePool capture_;ScratchPool scratch_;EventPool events_;rf::memory::PinnedRegion<std::int16_t> discard_;
    rf::runtime::HandleQueue<SlotIndex,Topology::CaptureQueueDepth> raw_queue_;
    rf::runtime::HandleQueue<SlotIndex,Topology::EventQueueDepth> event_queue_;
    Header headers_[Topology::CapturePoolDepth];
    SlotIndex held_capture_{InvalidSlot},held_event_{InvalidSlot};
    std::uint64_t sequence_{0},segment_{1},sample_position_{0},pending_lost_{0};
    std::uint32_t pending_gap_{0};bool ready_{false},have_hw_{false};std::int64_t last_hw_{0};std::size_t last_hw_count_{0};
    std::atomic<float> last_excess_{0};
};
}

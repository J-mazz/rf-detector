// Control-plane interpretation of atomic runtime observations; no worker access.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <atomic>
#include <cstdint>
#endif
export module rf.health;
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.runtime;
import rf.signal;

export namespace rf::health {

enum class Status { starting, calibrating, tracking, degraded, unavailable, stopping, stopped, failed };

inline const char* name(Status value) noexcept {
    switch (value) {
        case Status::starting: return "starting";
        case Status::calibrating: return "calibrating";
        case Status::tracking: return "tracking";
        case Status::degraded: return "degraded";
        case Status::unavailable: return "unavailable";
        case Status::stopping: return "stopping";
        case Status::stopped: return "stopped";
        case Status::failed: return "failed";
    }
    return "failed";
}

struct Snapshot {
    Status status;
    std::uint64_t timestamp_ns, capture_age_ns, dsp_age_ns;
    std::uint64_t received, admitted, processed, discarded, unknown_gaps;
    std::uint64_t windows, invalid_windows, resets, dropped_events;
    rf::signal::DetectorHealth detector;
    bool capture_stalled, dsp_stalled, sample_loss, device_gap, event_loss, invalid_input;
};

// One control-plane owner. Polls are approximate while workers run, and exact
// after joins. Interval flags clear after a poll; cumulative counters never do.
class Monitor {
public:
    Monitor(std::uint64_t start_ns, std::uint64_t stall_ns) noexcept
        : start_ns_(start_ns), stall_ns_(stall_ns) {}

    [[nodiscard]] Snapshot poll(const rf::runtime::StageState& state, std::uint64_t now_ns) noexcept {
        Snapshot h{};
        h.timestamp_ns = now_ns;
        h.detector = state.detector_health.load(std::memory_order_acquire);
        h.received = state.samples_received.load();
        h.admitted = state.samples_admitted.load();
        h.processed = state.samples_processed.load();
        h.discarded = state.samples_discarded.load();
        h.unknown_gaps = state.unknown_gap_events.load();
        h.windows = state.analysis_windows.load();
        h.invalid_windows = state.invalid_windows.load();
        h.resets = state.continuity_resets.load();
        h.dropped_events = state.events_dropped.load();
        h.capture_age_ns = age(now_ns, state.last_capture_ns.load(std::memory_order_acquire));
        const bool pending = h.admitted > h.processed;
        const auto progress_ns = state.last_dsp_ns.load(std::memory_order_acquire);
        // An idle stage has no deadline. Start one when pending work is first
        // observed, and restart on progress seen between polls. This deliberately
        // prefers a later fault report to counting idle time as a processing stall.
        if (pending && (!pending_ || h.processed != last_processed_)) pending_since_ns_ = now_ns;
        pending_ = pending;
        last_processed_ = h.processed;
        const auto baseline_ns = progress_ns > pending_since_ns_ ? progress_ns : pending_since_ns_;
        h.dsp_age_ns = pending ? age(now_ns, baseline_ns) : 0;
        h.capture_stalled = !state.acquisition_done.load(std::memory_order_acquire) && h.capture_age_ns >= stall_ns_;
        h.dsp_stalled = !state.dsp_done.load(std::memory_order_acquire) &&
                        pending && h.dsp_age_ns >= stall_ns_;
        h.sample_loss = h.discarded > last_discarded_;
        h.device_gap = h.unknown_gaps > last_gaps_;
        h.event_loss = h.dropped_events > last_events_dropped_;
        h.invalid_input = h.invalid_windows > last_invalid_;
        last_discarded_ = h.discarded;
        last_gaps_ = h.unknown_gaps;
        last_events_dropped_ = h.dropped_events;
        last_invalid_ = h.invalid_windows;

        if (state.fatal_error.load(std::memory_order_acquire) || state.protocol_violations.load())
            h.status = Status::failed;
        else if (state.sink_done.load(std::memory_order_acquire))
            h.status = Status::stopped;
        else if (state.stopping() || state.acquisition_done.load(std::memory_order_acquire))
            h.status = Status::stopping;
        else if (h.capture_stalled || h.dsp_stalled)
            h.status = Status::unavailable;
        else if (h.sample_loss || h.device_gap || h.event_loss || h.invalid_input ||
                 h.detector == rf::signal::DetectorHealth::uncertain)
            h.status = Status::degraded;
        else if (!h.received)
            h.status = Status::starting;
        else if (h.detector != rf::signal::DetectorHealth::tracking)
            h.status = Status::calibrating;
        else
            h.status = Status::tracking;
        return h;
    }

private:
    std::uint64_t age(std::uint64_t now_ns, std::uint64_t timestamp_ns) const noexcept {
        const auto reference = timestamp_ns ? timestamp_ns : start_ns_;
        return now_ns >= reference ? now_ns - reference : 0;
    }
    std::uint64_t start_ns_, stall_ns_;
    std::uint64_t last_discarded_{0}, last_gaps_{0}, last_events_dropped_{0}, last_invalid_{0};
    std::uint64_t pending_since_ns_{0}, last_processed_{0};
    bool pending_{false};
};
}

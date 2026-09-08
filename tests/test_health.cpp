#include <cstdio>
#ifndef RF_IMPORT_STD
#include <cstdlib>
#endif
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.runtime;
import rf.signal;
import rf.health;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c); std::exit(1); } } while (0)

int main() {
    using rf::health::Status;
    using rf::signal::DetectorHealth;
    rf::runtime::StageState state;
    rf::health::Monitor monitor(1000, 100);
    CHECK(monitor.poll(state, 1000).status == Status::starting);
    auto h = monitor.poll(state, 1100);
    CHECK(h.status == Status::unavailable && h.capture_stalled);
    // Recovery is based on delivered samples, not timeout/error counters alone.
    state.samples_received.add(256); state.samples_admitted.add(256);
    state.samples_processed.add(256);
    state.last_capture_ns.store(1110); state.last_dsp_ns.store(1110);
    CHECK(monitor.poll(state, 1110).status == Status::calibrating);
    state.detector_health.store(DetectorHealth::tracking);
    CHECK(monitor.poll(state, 1120).status == Status::tracking);
    CHECK(monitor.poll(state, 1210).status == Status::unavailable);
    state.last_capture_ns.store(1220); state.last_dsp_ns.store(1220);
    CHECK(monitor.poll(state, 1220).status == Status::tracking);

    state.samples_discarded.add(50); state.unknown_gap_events.add();
    state.events_dropped.add(); state.invalid_windows.add(); state.continuity_resets.add(3);
    h = monitor.poll(state, 1230);
    CHECK(h.status == Status::degraded);
    CHECK(h.sample_loss && h.device_gap && h.event_loss && h.invalid_input);
    CHECK(h.discarded == 50 && h.unknown_gaps == 1 && h.dropped_events == 1);
    CHECK(h.resets == 3);
    h = monitor.poll(state, 1240);
    CHECK(h.status == Status::tracking);
    CHECK(!h.sample_loss && !h.device_gap && !h.event_loss && !h.invalid_input);
    CHECK(h.discarded == 50); // cumulative loss remains visible after recovery
    state.detector_health.store(DetectorHealth::uncertain);
    CHECK(monitor.poll(state, 1250).status == Status::degraded);

    // Newer worker timestamps than the caller's sampled clock must not wrap.
    state.last_capture_ns.store(1300); state.last_dsp_ns.store(1300);
    state.detector_health.store(DetectorHealth::tracking);
    h = monitor.poll(state, 1290);
    CHECK(h.capture_age_ns == 0 && h.dsp_age_ns == 0 && h.status == Status::tracking);

    // Idle time is not a DSP stall: start the deadline when backlog is observed.
    state.samples_received.add(256); state.samples_admitted.add(256);
    state.last_capture_ns.store(1600);
    h = monitor.poll(state, 1600);
    CHECK(!h.dsp_stalled && h.dsp_age_ns == 0 && h.status == Status::tracking);
    // Continued capture cannot hide a DSP stall with admitted work pending.
    state.last_capture_ns.store(1700);
    h = monitor.poll(state, 1700);
    CHECK(h.dsp_stalled && !h.capture_stalled && h.status == Status::unavailable);
    state.samples_processed.add(256); state.last_dsp_ns.store(1710);
    CHECK(monitor.poll(state, 1710).status == Status::tracking);

    state.request_stop();
    CHECK(monitor.poll(state, 1500).status == Status::stopping);
    state.acquisition_done.store(true); state.dsp_done.store(true); state.sink_done.store(true);
    CHECK(monitor.poll(state, 1500).status == Status::stopped);
    state.fatal_error.store(true);
    CHECK(monitor.poll(state, 1500).status == Status::failed);
    state.fatal_error.store(false); state.protocol_violations.add();
    CHECK(monitor.poll(state, 1500).status == Status::failed);
    std::puts("test_health: PASS");
}

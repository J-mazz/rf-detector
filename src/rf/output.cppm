// Control-plane stream ownership and sink-thread event serialization.
module;
#include <cerrno>
#include <cstdio>
#ifndef RF_IMPORT_STD
#include <atomic>
#endif
#include <fcntl.h>
#include <unistd.h>
export module rf.output;
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.runtime;
import rf.signal;
import rf.health;

export namespace rf::output {

class CsvWriter {
public:
    explicit CsvWriter(rf::runtime::StageState& state) noexcept : state_(state) {}
    ~CsvWriter() noexcept { (void)close(); }
    CsvWriter(const CsvWriter&) = delete;
    CsvWriter& operator=(const CsvWriter&) = delete;

    // Main thread, before the sink starts. Existing paths are never overwritten.
    [[nodiscard]] bool open(const char* path) noexcept {
        if (file_) { errno = EBUSY; return false; }
        const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) return false;
        file_ = ::fdopen(fd, "w");
        if (!file_) { const int error = errno; ::close(fd); errno = error; return false; }
        // Every complete record leaves the stdio buffer while this call runs.
        // The sink remains isolated from acquisition/DSP; slow storage can
        // still cause explicit event drops and is not a hard real-time guarantee.
        if (std::setvbuf(file_, buffer_, _IOLBF, sizeof(buffer_)) != 0) return fail();
        if (std::fputs("event_id,segment_id,config_id,first_sample,end_sample,bin_begin,bin_end,frequency_hz,bandwidth_hz,power_db,background_db,excess_db,hw_time_valid,first_hw_time_ns,end_reason,phase\n", file_) < 0)
            return fail();
        return true;
    }

    // Main thread, after the sink joins; also used for setup-error cleanup.
    [[nodiscard]] bool close() noexcept {
        if (!file_) return true;
        FILE* file = file_; file_ = nullptr;
        return std::fclose(file) == 0 || fail();
    }

    static void callback(const rf::signal::CandidateEvent& event, void* context) noexcept {
        static_cast<CsvWriter*>(context)->write(event);
    }

private:
    void write(const rf::signal::CandidateEvent& e) noexcept {
        const auto& d = e.detection;
        if (!file_) { (void)fail(); return; }
        const int result = std::fprintf(file_, "%llu,%llu,%llu,%llu,%llu,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%lld,%u,%u\n",
            (unsigned long long)e.event_id, (unsigned long long)d.segment_id, (unsigned long long)e.source_meta.config_id,
            (unsigned long long)d.first_sample, (unsigned long long)d.end_sample, d.bin_begin, d.bin_end,
            e.source_meta.center_frequency_hz + d.center_offset_hz, d.bandwidth_hz, d.energy_db, d.noise_floor_db,
            d.excess_over_background_db, int(d.hw_time_valid), (long long)d.first_hw_time_ns,
            unsigned(d.end_reason), unsigned(d.phase));
        if (result < 0) (void)fail();
    }
    bool fail() noexcept {
        state_.fatal_error.store(true, std::memory_order_release);
        state_.request_stop();
        return false;
    }
    rf::runtime::StageState& state_;
    FILE* file_{nullptr};
    char buffer_[65536];
};

[[nodiscard]] inline bool write_health(FILE* file, const rf::health::Snapshot& h) noexcept {
    const int result = std::fprintf(file,
        "health state=%s timestamp_ns=%llu capture_age_ms=%.3f dsp_age_ms=%.3f "
        "received=%llu admitted=%llu processed=%llu discarded=%llu unknown_gaps=%llu "
        "windows=%llu invalid_windows=%llu resets=%llu dropped_events=%llu detector=%u "
        "capture_stalled=%d dsp_stalled=%d sample_loss=%d device_gap=%d event_loss=%d invalid_input=%d\n",
        rf::health::name(h.status), (unsigned long long)h.timestamp_ns,
        double(h.capture_age_ns) / 1e6, double(h.dsp_age_ns) / 1e6,
        (unsigned long long)h.received, (unsigned long long)h.admitted, (unsigned long long)h.processed,
        (unsigned long long)h.discarded, (unsigned long long)h.unknown_gaps,
        (unsigned long long)h.windows, (unsigned long long)h.invalid_windows, (unsigned long long)h.resets,
        (unsigned long long)h.dropped_events, unsigned(h.detector),
        int(h.capture_stalled), int(h.dsp_stalled), int(h.sample_loss), int(h.device_gap),
        int(h.event_loss), int(h.invalid_input));
    return result >= 0 && std::fflush(file) == 0;
}
}

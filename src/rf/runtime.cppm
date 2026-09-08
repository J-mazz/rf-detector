// rf.runtime — handle transport between stages, stage lifecycle state,
// telemetry counters, and thread placement.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <atomic>
#include <cstddef>
#include <cstdint>
#endif
#include <pthread.h>
#include <sched.h>

export module rf.runtime;

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.signal;

export namespace rf::runtime {

using rf::core::CacheLineSize;

// HandleQueue: SPSC transport for small trivially-copyable handles only.
// It never constructs, moves, or destroys objects, so `Capacity` live
// buffer elements cost exactly Capacity * sizeof(T) bytes and nothing
// else. Producer thread: try_push. Consumer thread: try_pop.
template <typename T, std::size_t Capacity>
    requires rf::core::Handle<T> && (Capacity >= 2) && (rf::core::is_power_of_two(Capacity))
class HandleQueue {
public:
    HandleQueue() noexcept = default;
    HandleQueue(const HandleQueue&) = delete;
    HandleQueue& operator=(const HandleQueue&) = delete;

    // Producer only.
    [[nodiscard]] bool try_push(T item) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= Capacity) return false;                 // saturated
        buffer_[w & Mask] = item;
        write_.store(w + 1, std::memory_order_release);      // publish after the write
        return true;
    }

    // Consumer only.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;                            // empty
        out = buffer_[r & Mask];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    // Cross-thread observer; acquire on both cursors. Only meaningful as a
    // snapshot — never as a shutdown criterion (use StageState flags).
    [[nodiscard]] std::size_t approx_size() const noexcept {
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t w = write_.load(std::memory_order_acquire);
        return w - r;
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t Mask = Capacity - 1;
    alignas(CacheLineSize) std::atomic<std::size_t> write_{0};
    alignas(CacheLineSize) std::atomic<std::size_t> read_{0};
    alignas(CacheLineSize) T buffer_[Capacity];
};

// One counter per cache line: the acquisition thread bumping rx_overruns
// must not false-share with the DSP thread bumping frames_processed.
struct alignas(CacheLineSize) Counter {
    std::atomic<std::uint64_t> value{0};
    void add(std::uint64_t n = 1) noexcept { value.fetch_add(n, std::memory_order_relaxed); }
    void set(std::uint64_t n) noexcept { value.store(n, std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t load() const noexcept { return value.load(std::memory_order_relaxed); }
};

// Stage lifecycle. Shutdown is a chain of "producer done" flags, each
// release-stored AFTER the producer's final push, so a consumer that
// acquire-loads the flag is guaranteed to see every item the producer
// published. `queue.empty()` is never a termination criterion.
struct StageState {
    std::atomic<bool> stop_requested{false};
    alignas(CacheLineSize) std::atomic<bool> acquisition_done{false};
    alignas(CacheLineSize) std::atomic<bool> dsp_done{false};
    alignas(CacheLineSize) std::atomic<bool> sink_done{false};

    std::atomic<bool> fatal_error{false};
    // Worker publication for the control-plane health observer. Zero means
    // that this stage has not yet delivered/progressed since startup.
    std::atomic<std::uint64_t> last_capture_ns{0};
    std::atomic<std::uint64_t> last_dsp_ns{0};
    std::atomic<rf::signal::DetectorHealth> detector_health{rf::signal::DetectorHealth::uncalibrated};
    Counter samples_received;
    Counter samples_admitted;
    Counter samples_processed;
    Counter samples_discarded;
    Counter unknown_gap_events;
    Counter malformed_reads;
    Counter analysis_windows;
    Counter invalid_windows;
    Counter continuity_resets;
    Counter events_dropped;
    Counter frames_captured;
    Counter frames_partial;        // reads that returned < capacity
    Counter rx_overruns;           // device-reported overflow
    Counter rx_timeouts;
    Counter rx_failures;
    Counter capture_pool_exhausted;
    Counter dsp_backpressure;      // capture→dsp queue full: frame dropped (slot retained)
    Counter frames_processed;
    Counter scratch_exhausted;
    Counter candidates;
    Counter events_emitted;
    Counter event_pool_exhausted;
    Counter event_backpressure;    // dsp→sink queue full: event overwritten next time
    Counter events_consumed;
    Counter protocol_violations;   // a release() returned false — must stay 0

    void request_stop() noexcept { stop_requested.store(true, std::memory_order_release); }
    [[nodiscard]] bool stopping() const noexcept { return stop_requested.load(std::memory_order_acquire); }
};

// Consumer-side drain policy shared by every stage:
//   1. pop if possible;
//   2. otherwise, if upstream is done, do one more pop (everything published
//      before the flag is now visible); if that fails we are finished;
//   3. otherwise relax and retry.
enum class Drain : std::uint8_t { item, finished, idle };

template <typename Q, typename T>
[[nodiscard]] inline Drain drain_step(Q& q, const std::atomic<bool>& upstream_done, T& out) noexcept {
    if (q.try_pop(out)) return Drain::item;
    if (upstream_done.load(std::memory_order_acquire)) {
        return q.try_pop(out) ? Drain::item : Drain::finished;
    }
    return Drain::idle;
}

inline void cpu_relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    asm volatile("" ::: "memory");
#endif
}

// Pin the calling thread to one CPU. Pair with isolcpus=/nohz_full= on the
// Pi so the dataplane cores are not shared with the scheduler's general pool.
[[nodiscard]] inline bool pin_current_thread(int cpu) noexcept {
    if (cpu < 0) return false;
    ::cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

// SCHED_FIFO for the acquisition thread. Needs CAP_SYS_NICE or an rtprio
// rlimit; failure is reported, not fatal.
[[nodiscard]] inline bool set_realtime_fifo(int priority) noexcept {
    ::sched_param sp{};
    sp.sched_priority = priority;
    return ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp) == 0;
}

} // namespace rf::runtime

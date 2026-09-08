// rf.core — shared primitive types, dataplane type contracts, clocks.
//
// Toolchain policy: GCC >= 16 (Fedora 44 ships 16.1.1) with -fmodules and
// `import std;` (RF_IMPORT_STD). The #ifndef branch below is a CI-only
// fallback for compilers without a std module (GCC 14/15); it is the sole
// reason standard headers ever appear in a global module fragment.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#endif
#include <time.h>

export module rf.core;

#ifdef RF_IMPORT_STD
import std;
#endif

export namespace rf::core {

// ---- handles ---------------------------------------------------------------

using SlotIndex = std::uint32_t;
inline constexpr SlotIndex InvalidSlot = std::numeric_limits<SlotIndex>::max();

// ---- physical quantities ---------------------------------------------------

using TimestampNs   = std::uint64_t;  // monotonic (steady_clock), ns
using WallNs        = std::int64_t;   // system_clock, ns since Unix epoch
using FrequencyHz   = double;
using SampleRateSps = double;

// L1D line on Cortex-A76 (Pi 5), Cortex-A78AE (Orin) and x86-64.
// Fixed on purpose: std::hardware_destructive_interference_size is an ABI
// hazard (GCC warns) and we only target these cores.
inline constexpr std::size_t CacheLineSize = 64;

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

// ---- dataplane type contracts ---------------------------------------------
//
// OverwriteRecord: storage for T is established once (start_lifetime_as),
// never constructed, never destroyed, and validity of contents is asserted
// explicitly by the producer. No default member initializers allowed —
// they would make the type non-trivially-default-constructible and hide
// an implicit 4 KiB memset behind every "construction".
template <typename T>
concept OverwriteRecord =
    std::is_trivially_copyable_v<T> &&
    std::is_trivially_default_constructible_v<T> &&
    std::is_trivially_destructible_v<T>;

// Handle: what may travel through a HandleQueue. Small and trivially
// copyable, so the queue never constructs, moves, or destroys objects.
template <typename T>
concept Handle = std::is_trivially_copyable_v<T> && (sizeof(T) <= 16);

// ---- clocks ---------------------------------------------------------------

// POSIX clocks: nanosecond units are part of the contract (clock_gettime),
// unlike steady_clock::duration::count(), and the calls are noexcept and
// allocation-free. Linux-only, like everything else in the dataplane.
[[nodiscard]] inline TimestampNs monotonic_now_ns() noexcept {
    ::timespec ts{};
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<TimestampNs>(ts.tv_sec) * 1'000'000'000ull + static_cast<TimestampNs>(ts.tv_nsec);
}

[[nodiscard]] inline WallNs wall_now_ns() noexcept {
    ::timespec ts{};
    (void)::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<WallNs>(ts.tv_sec) * 1'000'000'000ll + static_cast<WallNs>(ts.tv_nsec);
}

// Relative sleep for control-plane/mock pacing and idle worker backoff.
inline void sleep_ns(std::uint64_t ns) noexcept {
    ::timespec ts{};
    ts.tv_sec  = static_cast<time_t>(ns / 1'000'000'000ull);
    ts.tv_nsec = static_cast<long>(ns % 1'000'000'000ull);
    while (::nanosleep(&ts, &ts) != 0) { /* resume after EINTR */ }
}

// ---- dataplane error model (telemetry, never exceptions) -------------------

enum class RuntimeError : std::uint8_t {
    none,
    sdr_overrun,
    sdr_timeout,
    sdr_failure,
    pool_exhausted,
    queue_full,
    protocol_violation,   // SPSC role violated (double release, etc.)
    inference_failure,
    storage_failure,
    malformed_event
};

} // namespace rf::core

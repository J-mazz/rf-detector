// rf.memory — mmap-backed pinned storage with explicit object lifetime,
// an SPSC free list, and fixed pools built from both.
//
// Invariants (see DESIGN.md §3):
//   * allocation happens at startup only; the dataplane never allocates
//   * allocated capacity != constructed objects != valid samples
//   * every pool has exactly ONE acquiring thread and ONE releasing thread;
//     a stage that fails to hand a slot off KEEPS it (it never releases)
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#if defined(__cpp_lib_start_lifetime_as)
#include <memory>       // std::start_lifetime_as_array
#endif
#endif
#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

export module rf.memory;

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;

export namespace rf::memory {

using rf::core::CacheLineSize;
using rf::core::InvalidSlot;
using rf::core::SlotIndex;

enum class MemoryError : std::uint8_t {
    none,
    invalid_size,
    mmap_failed,
    mlock_failed,
    memlock_budget,      // RLIMIT_MEMLOCK too small for the requested locked bytes
    not_initialized
};

[[nodiscard]] inline std::size_t page_size() noexcept {
    const long v = ::sysconf(_SC_PAGESIZE);
    return v > 0 ? static_cast<std::size_t>(v) : 4096u;
}

// RLIMIT_MEMLOCK soft limit, or SIZE_MAX when unlimited.
[[nodiscard]] inline std::size_t memlock_limit_bytes() noexcept {
    ::rlimit rl{};
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) != 0) return 0;
    if (rl.rlim_cur == RLIM_INFINITY) return std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(rl.rlim_cur);
}

// Process-wide accounting of bytes this module has mlock()ed, so the
// budget check is against the *total*, not one region at a time.
inline std::atomic<std::size_t> locked_bytes_total{0};

// Establish an array of T in raw storage without running constructors.
// C++23 P2590R2 (GCC 16 implements it). The fallback is the pre-C++23
// idiom and is only compiled on toolchains that lack the library facility.
template <typename T>
    requires rf::core::OverwriteRecord<T>
[[nodiscard]] inline T* establish_array(void* storage, std::size_t count) noexcept {
#if defined(__cpp_lib_start_lifetime_as) && (__cpp_lib_start_lifetime_as >= 202207L)
    return std::start_lifetime_as_array<T>(storage, count);
#else
    (void)count;
    return static_cast<T*>(storage);
#endif
}

struct RegionConfig {
    bool lock;      // mlock() after mapping (prefaults; requires RLIMIT_MEMLOCK budget)
    bool populate;  // MAP_POPULATE when not locking
    bool huge;      // madvise(MADV_HUGEPAGE), best effort
};
inline constexpr RegionConfig DefaultRegionConfig{true, true, true};
inline constexpr RegionConfig UnlockedRegionConfig{false, true, true};

// One anonymous mapping holding `capacity` objects of T whose lifetime has
// been established but whose contents are indeterminate (kernel-zeroed once
// at mapping time; never re-zeroed on reuse).
template <typename T, std::size_t Alignment = CacheLineSize>
    requires rf::core::OverwriteRecord<T> &&
             (Alignment >= alignof(T)) &&
             (rf::core::is_power_of_two(Alignment)) &&
             (Alignment <= 4096)          // page alignment (>= 4 KiB on Linux) satisfies it
class PinnedRegion {
public:
    PinnedRegion() noexcept = default;
    ~PinnedRegion() noexcept { unmap(); }

    PinnedRegion(const PinnedRegion&) = delete;
    PinnedRegion& operator=(const PinnedRegion&) = delete;

    PinnedRegion(PinnedRegion&& o) noexcept
        : base_(std::exchange(o.base_, nullptr)),
          capacity_(std::exchange(o.capacity_, 0)),
          bytes_(std::exchange(o.bytes_, 0)),
          locked_(std::exchange(o.locked_, false)) {}

    PinnedRegion& operator=(PinnedRegion&& o) noexcept {
        if (this != &o) {
            unmap();
            base_     = std::exchange(o.base_, nullptr);
            capacity_ = std::exchange(o.capacity_, 0);
            bytes_    = std::exchange(o.bytes_, 0);
            locked_   = std::exchange(o.locked_, false);
        }
        return *this;
    }

    [[nodiscard]] MemoryError map(std::size_t capacity,
                                  RegionConfig cfg = DefaultRegionConfig) noexcept {
        unmap();
        if (capacity == 0 || capacity > std::numeric_limits<std::size_t>::max() / sizeof(T))
            return MemoryError::invalid_size;

        const std::size_t pg  = page_size();
        const std::size_t raw = capacity * sizeof(T);
        if (raw > std::numeric_limits<std::size_t>::max() - pg) return MemoryError::invalid_size;
        const std::size_t bytes = ((raw + pg - 1) / pg) * pg;

        if (cfg.lock) {
            const std::size_t limit = memlock_limit_bytes();
            const std::size_t inuse = locked_bytes_total.load(std::memory_order_relaxed);
            if (limit != std::numeric_limits<std::size_t>::max() && bytes > limit - inuse)
                return MemoryError::memlock_budget;
        }

        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (!cfg.lock && cfg.populate) flags |= MAP_POPULATE;   // mlock() prefaults anyway

        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (p == MAP_FAILED) return MemoryError::mmap_failed;

#ifdef MADV_HUGEPAGE
        if (cfg.huge) (void)::madvise(p, bytes, MADV_HUGEPAGE);
#endif
        if (cfg.lock) {
            // mmap(MAP_LOCKED) may silently leave pages unpopulated; mlock() is
            // the documented way to guarantee residency (mmap(2), mlock(2)).
            if (::mlock(p, bytes) != 0) {
                (void)::munmap(p, bytes);
                return MemoryError::mlock_failed;
            }
            locked_bytes_total.fetch_add(bytes, std::memory_order_relaxed);
        }

        base_     = establish_array<T>(p, capacity);
        capacity_ = capacity;
        bytes_    = bytes;
        locked_   = cfg.lock;
        return MemoryError::none;
    }

    void unmap() noexcept {
        if (base_ == nullptr) return;
        if (locked_) locked_bytes_total.fetch_sub(bytes_, std::memory_order_relaxed);
        (void)::munmap(static_cast<void*>(base_), bytes_);
        base_ = nullptr; capacity_ = 0; bytes_ = 0; locked_ = false;
    }

    [[nodiscard]] T*          data()     noexcept       { return base_; }
    [[nodiscard]] const T*    data()     const noexcept { return base_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t bytes()    const noexcept { return bytes_; }
    [[nodiscard]] bool        mapped()   const noexcept { return base_ != nullptr; }
    [[nodiscard]] bool        locked()   const noexcept { return locked_; }
    [[nodiscard]] std::span<T> span()    noexcept       { return {base_, capacity_}; }

private:
    T*          base_{nullptr};
    std::size_t capacity_{0};
    std::size_t bytes_{0};
    bool        locked_{false};
};

// SPSC free list: a pre-filled single-producer/single-consumer ring of slot
// indices. Exactly one thread calls try_acquire(); exactly one (possibly
// different) thread calls release(). Publication order is ring write, then
// release-store of the cursor — the defect in the previous revision was the
// reverse order.
template <std::size_t Slots>
    requires (Slots >= 2) && (rf::core::is_power_of_two(Slots))
class SPSCFreeList {
public:
    SPSCFreeList() noexcept {
        for (std::size_t i = 0; i < Slots; ++i) ring_[i] = static_cast<SlotIndex>(i);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(Slots, std::memory_order_release);
    }
    SPSCFreeList(const SPSCFreeList&) = delete;
    SPSCFreeList& operator=(const SPSCFreeList&) = delete;

    // Acquirer thread only.
    [[nodiscard]] SlotIndex try_acquire() noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return InvalidSlot;   // exhausted
        const SlotIndex s = ring_[h & Mask];
        head_.store(h + 1, std::memory_order_release);
        return s;
    }

    // Releaser thread only. Returns false on a protocol violation
    // (index out of range, or more releases than acquisitions).
    [[nodiscard]] bool release(SlotIndex s) noexcept {
        if (s >= Slots) return false;
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) >= Slots) return false;
        ring_[t & Mask] = s;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Cross-thread observers use acquire loads on both cursors.
    [[nodiscard]] std::size_t approx_free() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return t - h;
    }
    static constexpr std::size_t slots() noexcept { return Slots; }

private:
    static constexpr std::size_t Mask = Slots - 1;
    alignas(CacheLineSize) std::atomic<std::size_t> head_{0};   // acquirer's cursor
    alignas(CacheLineSize) std::atomic<std::size_t> tail_{0};   // releaser's cursor
    alignas(CacheLineSize) SlotIndex ring_[Slots];
};

// FixedPool: `Slots` slots of `SlotCapacity` T each, in ONE contiguous pinned
// region (one mapping → huge pages actually apply), fronted by an SPSC free
// list. initialize() is transactional: the pool is usable iff it returned
// MemoryError::none.
template <typename T, std::size_t SlotCapacity, std::size_t Slots,
          std::size_t Alignment = CacheLineSize>
    requires rf::core::OverwriteRecord<T> && (SlotCapacity >= 1) &&
             (Slots >= 2) && (rf::core::is_power_of_two(Slots))
class FixedPool {
    static_assert((SlotCapacity * sizeof(T)) % Alignment == 0,
                  "slot stride must preserve Alignment for every slot");
public:
    FixedPool() noexcept = default;
    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;

    [[nodiscard]] MemoryError initialize(RegionConfig cfg = DefaultRegionConfig) noexcept {
        return region_.map(SlotCapacity * Slots, cfg);
    }
    [[nodiscard]] bool ready() const noexcept { return region_.mapped(); }

    [[nodiscard]] SlotIndex try_acquire() noexcept { return free_.try_acquire(); }
    [[nodiscard]] bool      release(SlotIndex s) noexcept { return free_.release(s); }

    // Precondition: s < Slots and the caller currently owns s.
    [[nodiscard]] T* slot(SlotIndex s) noexcept {
        return region_.data() + static_cast<std::size_t>(s) * SlotCapacity;
    }
    [[nodiscard]] const T* slot(SlotIndex s) const noexcept {
        return region_.data() + static_cast<std::size_t>(s) * SlotCapacity;
    }

    [[nodiscard]] std::size_t approx_free() const noexcept { return free_.approx_free(); }

    static constexpr std::size_t slot_capacity() noexcept { return SlotCapacity; }
    static constexpr std::size_t slots()         noexcept { return Slots; }
    static constexpr std::size_t payload_bytes() noexcept { return SlotCapacity * Slots * sizeof(T); }

private:
    PinnedRegion<T, Alignment> region_;
    SPSCFreeList<Slots>        free_;
};

// Typed record pool: one T per slot, lifetime established once at startup,
// contents overwrite-only.
template <typename T, std::size_t Slots>
using ObjectPool = FixedPool<T, 1, Slots, CacheLineSize>;

} // namespace rf::memory

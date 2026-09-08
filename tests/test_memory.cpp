#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
// test_memory — rf.memory / rf.runtime primitives.
// Run under -fsanitize=thread: the stress section is the ordering proof.
#ifndef RF_IMPORT_STD
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <thread>
#endif

#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.memory;
import rf.runtime;
import rf.signal;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::exit(1); } } while (0)

using rf::core::InvalidSlot;
using rf::core::SlotIndex;

static void test_pinned_region() {
    rf::memory::PinnedRegion<float, 64> r;
    CHECK(!r.mapped());
    CHECK(r.map(0, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::invalid_size);
    CHECK(r.map(1000, rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    CHECK(r.mapped());
    CHECK(r.capacity() == 1000);
    CHECK(r.bytes() % rf::memory::page_size() == 0);
    CHECK(r.bytes() >= 1000 * sizeof(float));
    CHECK(reinterpret_cast<std::uintptr_t>(r.data()) % 64 == 0);
    r.data()[999] = 1.5f;
    CHECK(r.span()[999] == 1.5f);

    rf::memory::PinnedRegion<float, 64> moved(static_cast<rf::memory::PinnedRegion<float, 64>&&>(r));
    CHECK(!r.mapped() && moved.mapped() && moved.data()[999] == 1.5f);
    moved.unmap();
    CHECK(!moved.mapped() && moved.capacity() == 0);

    // Locked mapping: either succeeds within budget or reports the budget
    // honestly. A 64 GiB request must never succeed as a locked region.
    rf::memory::PinnedRegion<std::uint8_t, 64> huge;
    const auto e = huge.map(std::size_t{64} << 30, rf::memory::DefaultRegionConfig);
    CHECK(e == rf::memory::MemoryError::memlock_budget || e == rf::memory::MemoryError::mlock_failed ||
          e == rf::memory::MemoryError::mmap_failed);
    CHECK(rf::memory::locked_bytes_total.load() == 0);

    rf::memory::PinnedRegion<std::uint8_t, 64> small;
    const auto s = small.map(4096, rf::memory::DefaultRegionConfig);
    if (s == rf::memory::MemoryError::none) {
        CHECK(small.locked());
        CHECK(rf::memory::locked_bytes_total.load() == small.bytes());
        small.unmap();
        CHECK(rf::memory::locked_bytes_total.load() == 0);
    } else {
        std::printf("  (locked 4 KiB mapping unavailable here: %d — budget path exercised)\n", static_cast<int>(s));
    }
    std::puts("pinned region: ok");
}

static void test_freelist_single_thread() {
    rf::memory::SPSCFreeList<8> fl;
    CHECK(fl.approx_free() == 8);
    SlotIndex got[8];
    for (auto& g : got) { g = fl.try_acquire(); CHECK(g != InvalidSlot); }
    CHECK(fl.try_acquire() == InvalidSlot);          // exhaustion is a value, not a fault
    CHECK(fl.approx_free() == 0);
    bool seen[8] = {};
    for (auto g : got) { CHECK(g < 8); CHECK(!seen[g]); seen[g] = true; }
    CHECK(!fl.release(8));                            // out of range
    for (auto g : got) CHECK(fl.release(g));
    CHECK(!fl.release(0));                            // double release → protocol violation
    CHECK(fl.approx_free() == 8);
    std::puts("free list (single thread): ok");
}

static void test_region_and_pool_boundaries() {
    using Error=rf::memory::MemoryError;
    rf::memory::PinnedRegion<std::uint64_t> source,destination;
    CHECK(source.map(std::numeric_limits<std::size_t>::max(),rf::memory::UnlockedRegionConfig)==Error::invalid_size);
    rf::memory::PinnedRegion<std::uint8_t> bytes;
    CHECK(bytes.map(std::numeric_limits<std::size_t>::max(),rf::memory::UnlockedRegionConfig)==Error::invalid_size);
    CHECK(!bytes.mapped() && bytes.data()==nullptr && bytes.bytes()==0);
    CHECK(source.map(17,{false,false,true})==Error::none);
    source.data()[16]=123;
    CHECK(source.map(1,rf::memory::UnlockedRegionConfig)==Error::already_initialized);
    CHECK(destination.map(8,rf::memory::UnlockedRegionConfig)==Error::none);
    destination=std::move(source);
    CHECK(!source.mapped() && source.capacity()==0 && source.bytes()==0 && !source.locked());
    const auto& view=destination;
    CHECK(view.capacity()==17 && view.data()[16]==123);
    auto* alias=&destination;destination=std::move(*alias);
    CHECK(destination.capacity()==17 && destination.data()[16]==123);
    destination.unmap();destination.unmap();CHECK(destination.span().empty());

    rf::memory::FixedPool<std::uint64_t,8,4> pool;
    CHECK(!pool.ready() && !pool.release(0));
    CHECK(pool.initialize(rf::memory::UnlockedRegionConfig)==Error::none);
    CHECK(!pool.release(4) && !pool.release(InvalidSlot));
    for(SlotIndex index=0;index<4;++index){
        const auto slot=pool.try_acquire();CHECK(slot<4);
        CHECK(reinterpret_cast<std::uintptr_t>(pool.slot(slot))%rf::core::CacheLineSize==0);
        pool.slot(slot)[7]=100+slot;
    }
    const auto& const_pool=pool;
    for(SlotIndex index=0;index<4;++index){CHECK(const_pool.slot(index)[7]==100+index);CHECK(pool.release(index));}
    pool.reset_quiescent();CHECK(!pool.ready() && pool.try_acquire()==InvalidSlot);
    CHECK(pool.initialize(rf::memory::UnlockedRegionConfig)==Error::none && pool.approx_free()==4);

    rf::signal::EventTensor tensor;
    for(std::size_t hop=0;hop<tensor.Hops;++hop){
        for(std::size_t bin=0;bin<tensor.Bins;++bin)tensor.row(hop)[bin]=static_cast<std::int8_t>((hop+bin)%128);
    }
    for(std::size_t hop=0;hop<tensor.Hops;++hop)
        for(std::size_t bin=0;bin<tensor.Bins;++bin)
            CHECK(tensor.data[rf::signal::EventTensor::index(hop,bin)]==static_cast<std::int8_t>((hop+bin)%128));
}

static void test_queue_single_thread() {
    rf::runtime::HandleQueue<SlotIndex, 4> q;
    SlotIndex v = 0;
    CHECK(!q.try_pop(v));
    for (SlotIndex i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));                           // holds exactly Capacity items
    CHECK(q.approx_size() == 4);
    for (SlotIndex i = 0; i < 4; ++i) { CHECK(q.try_pop(v)); CHECK(v == i); }
    CHECK(!q.try_pop(v));
    // wrap-around across the mask many times
    for (SlotIndex i = 0; i < 1000; ++i) { CHECK(q.try_push(i)); CHECK(q.try_pop(v)); CHECK(v == i); }
    std::puts("handle queue (single thread): ok");
}

// Producer: acquire slot → stamp payload → push. Consumer: pop → verify
// payload → release. Exactly one acquirer and one releaser per pool, exactly
// one producer and one consumer per queue: the shipping topology in miniature.
static void test_cross_thread_stress() {
    constexpr std::size_t Slots = 16;
    constexpr std::size_t Words = 64;
    constexpr std::uint64_t Iterations = 300'000;

    rf::memory::FixedPool<std::uint64_t, Words, Slots> pool;
    CHECK(pool.initialize(rf::memory::UnlockedRegionConfig) == rf::memory::MemoryError::none);
    rf::runtime::HandleQueue<SlotIndex, 8> q;
    std::atomic<bool> producer_done{false};
    std::atomic<std::uint64_t> verified{0};
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> pool_dry{0};
    std::atomic<std::uint64_t> queue_full{0};
    std::atomic<std::uint64_t> bad{0};

    std::thread producer([&] {
        SlotIndex held = InvalidSlot;
        for (std::uint64_t it = 0; it < Iterations; ++it) {
            if (held == InvalidSlot) {
                held = pool.try_acquire();
                if (held == InvalidSlot) { pool_dry.fetch_add(1); std::this_thread::yield(); --it; continue; }
            }
            std::uint64_t* w = pool.slot(held);
            for (std::size_t k = 0; k < Words; ++k) w[k] = it * 1315423911ull + k;
            if (!q.try_push(held)) { queue_full.fetch_add(1); std::this_thread::yield(); --it; continue; }
            pushed.fetch_add(1);
            held = InvalidSlot;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        SlotIndex s = InvalidSlot;
        for (;;) {
            const auto d = rf::runtime::drain_step(q, producer_done, s);
            if (d == rf::runtime::Drain::finished) break;
            if (d == rf::runtime::Drain::idle) { std::this_thread::yield(); continue; }
            const std::uint64_t* w = pool.slot(s);
            const std::uint64_t base = w[0];
            for (std::size_t k = 0; k < Words; ++k) if (w[k] != base + k) bad.fetch_add(1);
            if (!pool.release(s)) bad.fetch_add(1);
            verified.fetch_add(1);
        }
    });

    producer.join();
    consumer.join();
    CHECK(bad.load() == 0);
    CHECK(pushed.load() == Iterations);
    CHECK(verified.load() == Iterations);
    CHECK(pool.approx_free() == Slots);
    std::printf("cross-thread stress: ok (%llu handoffs, pool_dry=%llu, queue_full=%llu)\n",
                (unsigned long long)verified.load(), (unsigned long long)pool_dry.load(),
                (unsigned long long)queue_full.load());
}

int main() {
    test_pinned_region();
    test_freelist_single_thread();
    test_region_and_pool_boundaries();
    test_queue_single_thread();
    test_cross_thread_stress();
    std::puts("test_memory: PASS");
    return 0;
}

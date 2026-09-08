#include <cstdio>
#include <pthread.h>
#include <sched.h>
#ifndef RF_IMPORT_STD
#include <atomic>
#include <cstdlib>
#include <cstdint>
#endif
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.runtime;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c); std::exit(1); } } while (0)

// Model publication between the first failed pop and the done-flag load.
struct FinalPublication {
    std::atomic<bool>& done;
    unsigned calls=0;
    bool try_pop(std::uint64_t& out) noexcept {
        if (++calls==1) { done.store(true, std::memory_order_release); return false; }
        out=42; return true;
    }
};

int main() {
    using namespace rf::runtime;
    HandleQueue<std::uint64_t, 2> queue{};
    CHECK(queue.capacity()==2 && queue.approx_size()==0);
    std::uint64_t value=99;
    CHECK(!queue.try_pop(value) && value==99);
    // Repeatedly wrap the storage, including a rejected write to a full queue.
    for (std::uint64_t n=0; n<1000; n+=2) {
        CHECK(queue.try_push(n) && queue.try_push(n+1));
        CHECK(queue.approx_size()==2 && !queue.try_push(9999));
        CHECK(queue.try_pop(value) && value==n);
        CHECK(queue.approx_size()==1);
        CHECK(queue.try_pop(value) && value==n+1);
        CHECK(queue.approx_size()==0);
    }
    std::atomic<bool> done{false};
    CHECK(drain_step(queue,done,value)==Drain::idle);
    CHECK(queue.try_push(7));
    CHECK(drain_step(queue,done,value)==Drain::item && value==7);
    CHECK(queue.try_push(8)); done.store(true);
    CHECK(drain_step(queue,done,value)==Drain::item && value==8);
    CHECK(drain_step(queue,done,value)==Drain::finished);
    done.store(false);
    FinalPublication publication{done};
    CHECK(drain_step(publication,done,value)==Drain::item && value==42);
    CHECK(publication.calls==2);
    StageState state;
    CHECK(!state.stopping()); state.request_stop(); state.request_stop();
    CHECK(state.stopping() && !state.acquisition_done && !state.dsp_done && !state.sink_done);
    CHECK(!state.fatal_error);
    Counter count; CHECK(count.load()==0); count.add(); count.add(5);
    CHECK(count.load()==6); count.set(3); CHECK(count.load()==3);
    CHECK(!pin_current_thread(-1));
    CHECK(!set_realtime_fifo(-1));
    cpu_set_t original;CHECK(pthread_getaffinity_np(pthread_self(),sizeof(original),&original)==0);
    int cpu=0;while(cpu<CPU_SETSIZE && !CPU_ISSET(cpu,&original))++cpu;
    CHECK(cpu<CPU_SETSIZE && pin_current_thread(cpu));
    cpu_set_t pinned;CHECK(pthread_getaffinity_np(pthread_self(),sizeof(pinned),&pinned)==0);
    CHECK(CPU_COUNT(&pinned)==1 && CPU_ISSET(cpu,&pinned));
    CHECK(pthread_setaffinity_np(pthread_self(),sizeof(original),&original)==0);
    std::puts("test_runtime: PASS");
}

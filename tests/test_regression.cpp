#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
#ifndef RF_IMPORT_STD
#include <cstdio>
#include <cstdlib>
#endif
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.memory;
import rf.core;
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"failed: %s:%d: %s\n",__FILE__,__LINE__,#c);std::exit(1);}}while(0)
int main(){
    rf::memory::SPSCFreeList<4> fl;
    const auto a=fl.try_acquire(); const auto b=fl.try_acquire();(void)b;
    CHECK(fl.release(a)); CHECK(!fl.release(a));
    rf::memory::FixedPool<float,16,4> p;
    CHECK(p.try_acquire()==rf::core::InvalidSlot);
    CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
    const auto s=p.try_acquire();p.slot(s)[0]=42;
    CHECK(p.initialize(rf::memory::UnlockedRegionConfig)!=rf::memory::MemoryError::none);
    CHECK(p.slot(s)[0]==42);CHECK(p.release(s));
    std::puts("test_regression: PASS");
}

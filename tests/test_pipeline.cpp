#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
#include <cstdint>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#endif
#include <sys/resource.h>
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.acquire;
import rf.pipeline;
import rf.runtime;
import rf.memory;
import rf.core;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c);std::exit(1);}}while(0)
static rf::acquire::ReceiverConfig config(){return {800e6,30.72e6,30e6,40,7};}
class FiniteSource final:public rf::acquire::SDRSource {
public:
    std::size_t read=0;bool errors=false,abrupt=false;rf::acquire::ReceiverConfig c=::config();
    bool configure(const rf::acquire::ReceiverConfig& v) noexcept override{c=v;return true;}
    bool start() noexcept override{return true;}void stop() noexcept override{}
    const rf::acquire::ReceiverConfig& config() const noexcept override{return c;}
    rf::acquire::ReadResult read_into(std::int16_t* dst,std::size_t cap,std::uint32_t) noexcept override {
        if(errors)return {0,rf::acquire::ReadStatus::failure,0,false};
        if(read++==64)return {0,rf::acquire::ReadStatus::stopped,0,false};
        const auto count=std::min<std::size_t>(512,cap);std::fill_n(dst,count*2,std::int16_t{32});
        if(abrupt && read==1)return {count,rf::acquire::ReadStatus::overflow,0,false};
        return {count,rf::acquire::ReadStatus::ok,0,false};
    }
};
class RecoveringSource final:public rf::acquire::SDRSource {
public:
    explicit RecoveringSource(const rf::runtime::StageState& state):state_(state){}
    bool configure(const rf::acquire::ReceiverConfig&) noexcept override{return true;}
    bool start() noexcept override{return true;}
    void stop() noexcept override{}
    const rf::acquire::ReceiverConfig& config() const noexcept override{return config_;}
    rf::acquire::ReadResult read_into(std::int16_t* dst,std::size_t cap,std::uint32_t) noexcept override {
        // Neither timeouts nor overflows can publish a usable-capture timestamp.
        if(read_<=6)CHECK(state_.last_capture_ns.load()==0);
        if(read_++<3)return {0,rf::acquire::ReadStatus::timeout,0,false};
        if(read_<=6)return {0,rf::acquire::ReadStatus::overflow,0,false};
        if(read_>7)return {0,rf::acquire::ReadStatus::stopped,0,false};
        const auto count=std::min<std::size_t>(512,cap);
        std::fill_n(dst,2*count,std::int16_t{32});
        return {count,rf::acquire::ReadStatus::ok,0,false};
    }
private:
    const rf::runtime::StageState& state_;
    rf::acquire::ReceiverConfig config_=::config();
    unsigned read_=0;
};
static void drained(const rf::pipeline::PipelineSystem& p,const rf::runtime::StageState& s){
    CHECK(s.frames_processed.load()==s.frames_captured.load());CHECK(s.samples_processed.load()==s.samples_admitted.load());
    CHECK(s.samples_received.load()==s.samples_admitted.load()+s.samples_discarded.load());
    CHECK(s.events_consumed.load()==s.events_emitted.load());
    CHECK(s.candidates.load()==s.events_emitted.load()+s.events_dropped.load());
    CHECK(s.protocol_violations.load()==0);
    CHECK(p.capture_slots_free()==rf::pipeline::Topology::CapturePoolDepth);
    CHECK(p.scratch_slots_free()==rf::pipeline::Topology::ScratchDepth);
    CHECK(p.event_slots_free()==rf::pipeline::Topology::EventPoolDepth);
}
int main(){
    {
        rf::runtime::StageState s;RecoveringSource source(s);rf::pipeline::PipelineSystem p(source,s);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        p.run_acquisition();
        CHECK(s.rx_timeouts.load()==3 && s.rx_overruns.load()==3);
        CHECK(s.last_capture_ns.load()>0 && s.last_dsp_ns.load()==0);
        CHECK(s.samples_received.load()==512 && s.samples_discarded.load()==0);
        p.run_dsp();p.run_sink();p.reclaim_held_slots();
        CHECK(s.last_dsp_ns.load()>0);CHECK(!s.fatal_error.load());drained(p,s);
    }
    {
        FiniteSource source;rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::already_initialized);CHECK(p.ready());
        p.run_acquisition(); // DSP deliberately absent: finite input must still drain.
        CHECK(s.samples_received.load()==32768);CHECK(s.samples_admitted.load()==8192);CHECK(s.samples_discarded.load()==24576);
        CHECK(s.rx_overruns.load()==0);CHECK(s.unknown_gap_events.load()==0);
        std::thread dsp([&]{p.run_dsp();});std::thread sink([&]{p.run_sink();});dsp.join();sink.join();
        p.reclaim_held_slots();drained(p,s);
    }
    {
        FiniteSource source;source.abrupt=true;rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        p.run_acquisition();p.run_dsp();p.run_sink();p.reclaim_held_slots();
        CHECK(s.samples_received.load()==32768);CHECK(s.samples_admitted.load()==8192);
        CHECK(s.samples_discarded.load()==24576);CHECK(s.rx_overruns.load()==1);
        CHECK(s.unknown_gap_events.load()==1);drained(p,s);
    }
    {
        FiniteSource source;source.errors=true;rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        p.run_acquisition();p.run_dsp();p.run_sink();p.reclaim_held_slots();
        CHECK(s.fatal_error.load());CHECK(s.rx_failures.load()==8);drained(p,s);
    }
    {
        struct rlimit saved{};CHECK(getrlimit(RLIMIT_MEMLOCK,&saved)==0);
        auto limited=saved;limited.rlim_cur=std::min<rlim_t>(saved.rlim_cur,8388608);
        CHECK(setrlimit(RLIMIT_MEMLOCK,&limited)==0);
        FiniteSource source;rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        const auto baseline=rf::memory::locked_bytes_total.load();
        CHECK(p.initialize()!=rf::memory::MemoryError::none);CHECK(!p.ready());
        CHECK(rf::memory::locked_bytes_total.load()==baseline);CHECK(setrlimit(RLIMIT_MEMLOCK,&saved)==0);
    }
    {
        rf::acquire::MockSource source(true);CHECK(source.configure(config()));rf::runtime::StageState s;
        rf::pipeline::PipelineSystem p(source,s);CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);CHECK(source.start());
        std::thread acq([&]{p.run_acquisition();});std::thread dsp([&]{p.run_dsp();});std::thread sink([&]{p.run_sink();});
        rf::core::sleep_ns(250000000);
        CHECK(!s.acquisition_done.load() && !s.dsp_done.load());
        CHECK(s.last_capture_ns.load()>0 && s.last_dsp_ns.load()>0);
        CHECK(s.samples_processed.load()>0 && s.analysis_windows.load()>0);
        s.request_stop();acq.join();dsp.join();sink.join();source.stop();p.reclaim_held_slots();
        drained(p,s);CHECK(!s.fatal_error.load());CHECK(s.analysis_windows.load()>0);CHECK(s.candidates.load()>0);
    }
    std::puts("test_pipeline: PASS");
}

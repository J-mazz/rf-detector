#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
#include <cstdint>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
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
import rf.signal;
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
class ScriptedSource final:public rf::acquire::SDRSource {
public:
    std::vector<rf::acquire::ReadResult> reads;
    std::size_t position=0;bool tone=false;
    rf::acquire::ReceiverConfig c{915e6,256000,250000,10,42};
    bool configure(const rf::acquire::ReceiverConfig& value) noexcept override{c=value;return true;}
    bool start() noexcept override{return true;}void stop() noexcept override{}
    const rf::acquire::ReceiverConfig& config() const noexcept override{return c;}
    rf::acquire::ReadResult read_into(std::int16_t* dst,std::size_t capacity,std::uint32_t) noexcept override{
        if(position==reads.size())return {0,rf::acquire::ReadStatus::stopped,0,false};
        const auto result=reads[position++];
        for(std::size_t k=0;k<std::min(capacity,result.complex_samples);++k){
            const double phase=2*3.14159265358979323846*23*k/256;
            dst[2*k]=static_cast<std::int16_t>(int(k*17%101)-50+(tone && k>=4*256?2000*std::cos(phase):0));
            dst[2*k+1]=static_cast<std::int16_t>(int(k*7%103)-51+(tone && k>=4*256?2000*std::sin(phase):0));
        }
        return result;
    }
};
static void run_serial(rf::pipeline::PipelineSystem& p){
    p.run_acquisition();p.run_dsp();p.run_sink();p.reclaim_held_slots();p.reclaim_held_slots();
}
static void failure_and_timestamp_boundaries(){
    using Status=rf::acquire::ReadStatus;
    using Error=rf::memory::MemoryError;
    {
        ScriptedSource source;rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        run_serial(p);CHECK(s.fatal_error && s.stopping() && s.acquisition_done && s.dsp_done && s.sink_done);
        CHECK(source.position==0);
    }
    for(int mode=0;mode<4;++mode){
        ScriptedSource source;rf::runtime::StageState s;rf::pipeline::PipelineConfig cfg;
        if(mode==0)cfg.read_timeout_us=0;
        if(mode==1)cfg.max_consecutive_failures=0;
        if(mode==2)source.c.sample_scale=0;
        if(mode==3)cfg.detector.hop=0;
        rf::pipeline::PipelineSystem p(source,s,cfg);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==Error::invalid_size && !p.ready());
        CHECK(p.capture_slots_free()==rf::pipeline::Topology::CapturePoolDepth);
    }
    {
        ScriptedSource source;source.reads={{rf::pipeline::Topology::FrameComplex+1,Status::ok,0,false}};
        rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==Error::none);run_serial(p);
        CHECK(s.fatal_error && s.malformed_reads.load()==1 && s.samples_received.load()==0);drained(p,s);
    }
    {
        ScriptedSource source;source.reads={{0,Status::failure,0,false},{256,Status::ok,0,false},
            {0,Status::failure,0,false},{256,Status::ok,0,false},{0,Status::ok,0,false}};
        rf::runtime::StageState s;rf::pipeline::PipelineConfig cfg;cfg.max_consecutive_failures=2;
        rf::pipeline::PipelineSystem p(source,s,cfg);CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==Error::none);
        run_serial(p);CHECK(!s.fatal_error && s.rx_failures.load()==2 && s.unknown_gap_events.load()==2);
        CHECK(s.samples_received.load()==512 && s.frames_captured.load()==2);drained(p,s);
    }
    for(const std::int64_t delta:{-3,-2,0,2,3}){
        ScriptedSource source;source.c.sample_rate_sps=1e9;source.c.bandwidth_hz=900e6;
        source.reads={{256,Status::ok,0,true},{256,Status::ok,256+delta,true}};
        rf::runtime::StageState s;rf::pipeline::PipelineConfig cfg;cfg.detector.fft_size=256;cfg.detector.hop=128;
        rf::pipeline::PipelineSystem p(source,s,cfg);CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==Error::none);
        run_serial(p);const bool gap=std::abs(delta)>2;
        CHECK(!s.fatal_error && s.unknown_gap_events.load()==unsigned(gap));
        CHECK(s.continuity_resets.load()==unsigned(gap));CHECK(s.analysis_windows.load()==(gap?2u:3u));drained(p,s);
    }
    {
        ScriptedSource source;source.reads={{256,Status::ok,0,true},{256,Status::ok,0,false},{256,Status::ok,9000000,true}};
        rf::runtime::StageState s;rf::pipeline::PipelineSystem p(source,s);
        CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==Error::none);run_serial(p);
        CHECK(s.unknown_gap_events.load()==0 && !s.fatal_error);drained(p,s);
    }
}
struct SinkRecords {
    std::size_t count=0;float last_excess=0;
    static void collect(const rf::signal::CandidateEvent& e,void* context) noexcept{
        auto& out=*static_cast<SinkRecords*>(context);++out.count;
        CHECK(e.event_id==e.detection.event_id && e.source_meta.receiver_id==42);
        CHECK(e.source_meta.sample_rate_sps==256000 && e.source_meta.center_frequency_hz==915e6);
        CHECK(e.timestamp_ns>0 && e.wall_ns>0 && e.tensor_valid_hops==0 && e.quant.scale==0);
        CHECK(e.detection.bin_begin==151 && e.detection.bin_end==152);
        out.last_excess=e.detection.excess_over_background_db;
    }
};
static void delayed_sink_accounting(){
    ScriptedSource source;source.tone=true;source.reads={{32768,rf::acquire::ReadStatus::ok,0,true}};
    rf::runtime::StageState s;rf::pipeline::PipelineConfig cfg;SinkRecords records;
    cfg.detector.fft_size=256;cfg.detector.hop=256;cfg.detector.band_bins=1;
    cfg.detector.reference_bands=2;cfg.detector.guard_bands=1;cfg.detector.warmup_windows=3;
    cfg.detector.update_windows=1;cfg.detector.range_count=1;cfg.detector.ranges[0]={151,152};
    cfg.sink=SinkRecords::collect;cfg.sink_context=&records;
    rf::pipeline::PipelineSystem p(source,s,cfg);CHECK(p.initialize(rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
    p.run_acquisition();p.run_dsp();
    CHECK(s.events_consumed.load()==0 && s.events_emitted.load()==rf::pipeline::Topology::EventPoolDepth);
    CHECK(s.events_dropped.load()>0 && s.events_dropped.load()==s.event_pool_exhausted.load());
    CHECK(s.samples_discarded.load()==0 && !s.fatal_error);
    p.run_sink();p.reclaim_held_slots();drained(p,s);
    CHECK(records.count==s.events_emitted.load() && p.last_excess_db()==records.last_excess);
    CHECK(std::isfinite(p.noise_floor_db()));
}
int main(){
    failure_and_timestamp_boundaries();delayed_sink_accounting();
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

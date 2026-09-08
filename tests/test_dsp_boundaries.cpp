#include <cstdio>
#include <new>
#include <sys/resource.h>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>
#endif
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.dsp;
import rf.signal;
import rf.memory;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c); std::exit(1); } } while (0)
using Config=rf::dsp::DetectorConfig;
using Meta=rf::signal::CaptureMetadata;
using Error=rf::memory::MemoryError;
using Phase=rf::signal::EventPhase;
using End=rf::signal::EventEnd;
constexpr std::size_t N=256;
struct Record { rf::signal::SpectralCandidate event; Meta meta; };
static void collect(const rf::signal::SpectralCandidate& event,const Meta& meta,void* context) noexcept {
    static_cast<std::vector<Record>*>(context)->push_back({event,meta});
}
static Config config() {
    Config c; c.fft_size=N; c.hop=N; c.band_bins=1; c.reference_bands=2;
    c.guard_bands=1; c.edge_bins=4; c.dc_bins=1; c.warmup_windows=3;
    c.range_count=1; c.ranges[0]={151,152}; c.update_windows=2; return c;
}
static Meta metadata() {
    auto m=Meta::zero(); m.sample_rate_sps=256000; m.bandwidth_hz=250000;
    m.center_frequency_hz=915e6; m.segment_id=7; m.config_id=9;
    m.hw_time_valid=true; m.hw_time_ns=1000000; return m;
}
struct Fixture {
    rf::dsp::StreamingDetector detector;
    std::vector<float> i,q;
    std::vector<Record> records;
    Fixture():i(10*N),q(i.size()) {
        CHECK(detector.initialize(config(),rf::memory::UnlockedRegionConfig)==Error::none);
        for (std::size_t frame=0; frame<10; ++frame) {
            std::uint64_t rng=173;
            for (std::size_t x=0; x<N; ++x) {
                auto noise=[&] { rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return float(int(rng%2001)-1000)*0.000002f; };
                const double phase=2*3.14159265358979323846*23*x/N;
                i[frame*N+x]=noise()+(frame>=4?0.01f*float(std::cos(phase)):0);
                q[frame*N+x]=noise()+(frame>=4?0.01f*float(std::sin(phase)):0);
            }
        }
    }
    void feed(Meta meta,std::size_t count=10*N) {
        rf::signal::PlanarIQBlock b{i.data(),q.data(),i.size(),count,meta};
        CHECK(detector.feed(b,collect,&records));
    }
};

static void configuration_validation() {
    using Change=void(*)(Config&);
    const Change invalid[]={
        +[](Config& c){c.fft_size=128;}, +[](Config& c){c.fft_size=131072;},
        +[](Config& c){c.fft_size=257;}, +[](Config& c){c.hop=0;}, +[](Config& c){c.hop=N+1;},
        +[](Config& c){c.band_bins=0;}, +[](Config& c){c.band_bins=3;}, +[](Config& c){c.band_bins=64;},
        +[](Config& c){c.reference_bands=0;}, +[](Config& c){c.reference_bands=65;},
        +[](Config& c){c.guard_bands=65;}, +[](Config& c){c.warmup_windows=0;},
        +[](Config& c){c.warmup_windows=1000001;}, +[](Config& c){c.end_hold_windows=0;},
        +[](Config& c){c.end_hold_windows=1000001;}, +[](Config& c){c.update_windows=0;},
        +[](Config& c){c.update_windows=1000001;}, +[](Config& c){c.range_count=17;},
        +[](Config& c){c.edge_bins=N/4;}, +[](Config& c){c.dc_bins=N/4;},
        +[](Config& c){c.threshold_db=std::numeric_limits<float>::quiet_NaN();},
        +[](Config& c){c.release_db=std::numeric_limits<float>::infinity();},
        +[](Config& c){c.release_db=0;}, +[](Config& c){c.threshold_db=c.release_db;},
        +[](Config& c){c.max_clipped_fraction=-0.1f;}, +[](Config& c){c.max_clipped_fraction=1;},
        +[](Config& c){c.max_clipped_fraction=std::numeric_limits<float>::quiet_NaN();},
        +[](Config& c){c.floor_shift_db=0;}, +[](Config& c){c.floor_shift_db=std::numeric_limits<float>::infinity();},
        +[](Config& c){c.ranges[0]={5,5};}, +[](Config& c){c.ranges[0]={6,5};},
        +[](Config& c){c.ranges[0]={0,N+1};}
    };
    for (auto change:invalid) {
        rf::dsp::StreamingDetector d; auto c=config(); change(c);
        CHECK(d.initialize(c,rf::memory::UnlockedRegionConfig)==Error::invalid_size);
        CHECK(d.initialize(config(),rf::memory::UnlockedRegionConfig)==Error::none);
        CHECK(d.initialize(c,rf::memory::UnlockedRegionConfig)==Error::already_initialized);
    }
    // Extremal valid FFT sizes and a one-sample hop must be accepted.
    for (const auto n:{N,std::size_t{65536}}) {
        rf::dsp::StreamingDetector d; auto c=config(); c.fft_size=n; c.hop=1;
        c.max_clipped_fraction=0; c.guard_bands=0; c.edge_bins=0; c.dc_bins=0;
        CHECK(d.initialize(c,rf::memory::UnlockedRegionConfig)==Error::none);
        d.reset(); d.reset(); CHECK(d.initialize(config(),rf::memory::UnlockedRegionConfig)==Error::none);
    }
}

static void feed_validation() {
    Fixture f;
    rf::signal::PlanarIQBlock good{f.i.data(),f.q.data(),N,N,metadata()};
    rf::dsp::StreamingDetector uninitialized;
    CHECK(!uninitialized.feed(good,collect,&f.records));
    CHECK(!f.detector.feed(good,nullptr,&f.records));
    using Change=void(*)(rf::signal::PlanarIQBlock&);
    const Change invalid[]={
        +[](rf::signal::PlanarIQBlock& b){b.count_complex=N+1;}, +[](rf::signal::PlanarIQBlock& b){b.i=nullptr;}, +[](rf::signal::PlanarIQBlock& b){b.q=nullptr;},
        +[](rf::signal::PlanarIQBlock& b){b.meta.sample_rate_sps=0;}, +[](rf::signal::PlanarIQBlock& b){b.meta.sample_rate_sps=1e9+1;},
        +[](rf::signal::PlanarIQBlock& b){b.meta.sample_rate_sps=std::numeric_limits<double>::quiet_NaN();},
        +[](rf::signal::PlanarIQBlock& b){b.meta.bandwidth_hz=0;}, +[](rf::signal::PlanarIQBlock& b){b.meta.bandwidth_hz=b.meta.sample_rate_sps+1;},
        +[](rf::signal::PlanarIQBlock& b){b.meta.bandwidth_hz=std::numeric_limits<double>::infinity();},
        +[](rf::signal::PlanarIQBlock& b){b.meta.center_frequency_hz=std::numeric_limits<double>::quiet_NaN();},
        +[](rf::signal::PlanarIQBlock& b){b.meta.gain_db=std::numeric_limits<float>::infinity();},
        +[](rf::signal::PlanarIQBlock& b){b.meta.first_sample=std::numeric_limits<std::uint64_t>::max()-N+1;}
    };
    for (auto change:invalid) { auto b=good; change(b); CHECK(!f.detector.feed(b,collect,&f.records)); }
    auto empty=good; empty.i=empty.q=nullptr; empty.count_complex=empty.capacity_complex=0;
    CHECK(f.detector.feed(empty,collect,&f.records)); CHECK(f.detector.windows_processed()==0);
    CHECK(f.detector.feed(good,collect,&f.records)); CHECK(f.detector.windows_processed()==1);
    CHECK(f.detector.continuity_resets()==0 && f.records.empty());
}

static void allocation_rollback() {
    struct rlimit saved{};CHECK(getrlimit(RLIMIT_MEMLOCK,&saved)==0);
    const auto baseline=rf::memory::locked_bytes_total.load();
    // Sweep small budgets through successive FFT/scratch/band allocations.
    // Every failed initialization must undo ALL earlier locked mappings.
    for(std::size_t pages=0;pages<16;++pages){
        const auto budget=pages*rf::memory::page_size();
        if(budget>saved.rlim_cur)break;
        auto limited=saved;limited.rlim_cur=budget;CHECK(setrlimit(RLIMIT_MEMLOCK,&limited)==0);
        rf::dsp::StreamingDetector d;const auto result=d.initialize(config());
        if(result==Error::none)d.reset();
        else CHECK(result==Error::memlock_budget || result==Error::mlock_failed || result==Error::mmap_failed);
        CHECK(rf::memory::locked_bytes_total.load()==baseline);
        CHECK(setrlimit(RLIMIT_MEMLOCK,&saved)==0);
        CHECK(d.initialize(config(),rf::memory::UnlockedRegionConfig)==Error::none);
    }
}

static void continuity_and_cache() {
    using Change=void(*)(Meta&);
    const Change changes[]={
        +[](Meta& m){++m.segment_id;}, +[](Meta& m){++m.config_id;},
        +[](Meta& m){++m.first_sample;}, +[](Meta& m){m.sample_rate_sps*=2;},
        +[](Meta& m){m.center_frequency_hz+=1e6;}, +[](Meta& m){m.bandwidth_hz=100000;},
        +[](Meta& m){m.gain_db+=1;}, +[](Meta& m){m.gap_flags=rf::signal::GapDiscard;}
    };
    for (auto change:changes) {
        Fixture f; auto m=metadata(); f.feed(m);
        CHECK(!f.records.empty() && f.records.front().event.phase==Phase::begin);
        const auto id=f.records.front().event.event_id;
        f.records.clear(); m.first_sample=10*N; change(m); f.feed(m,N/2);
        CHECK(f.detector.continuity_resets()==1 && f.detector.windows_processed()==10);
        CHECK(f.detector.health()==rf::signal::DetectorHealth::uncalibrated);
        CHECK(f.records.size()==1);
        const auto& end=f.records[0];
        CHECK(end.event.event_id==id && end.event.phase==Phase::end && end.event.end_reason==End::gap);
        CHECK(end.event.end_sample==10*N && end.meta.sample_rate_sps==256000 && end.meta.config_id==9);
        CHECK(end.event.center_offset_hz==23000 && end.event.bandwidth_hz==1000);
        f.detector.finish(collect,&f.records); CHECK(f.records.size()==1);
    }
    // Change bandwidth and rate on the SAME detector; bin 151 must leave and
    // re-enter the usable spectrum, and published Hz/duration must use new units.
    Fixture f; auto m=metadata(); f.feed(m); CHECK(!f.records.empty());
    f.detector.finish(collect,&f.records); f.records.clear();
    m.bandwidth_hz=40000; f.feed(m); f.detector.finish(collect,&f.records);
    CHECK(f.records.empty());
    m.sample_rate_sps=128000; f.feed(m); f.detector.finish(collect,&f.records);
    CHECK(!f.records.empty());
    for (const auto& r:f.records) {
        CHECK(r.event.bin_begin==151 && r.event.bin_end==152);
        CHECK(r.event.center_offset_hz==11500 && r.event.bandwidth_hz==500);
        CHECK(std::fabs(r.event.duration_s-float(r.event.end_sample-r.event.first_sample)/128000)<1e-7f);
    }
    f.records.clear(); m.sample_rate_sps=256000; f.feed(m); f.detector.finish(collect,&f.records);
    CHECK(f.records.empty());
    m.bandwidth_hz=250000; f.feed(m); f.detector.finish(collect,&f.records); CHECK(!f.records.empty());
    const auto size=f.records.size(); f.detector.finish(collect,&f.records); CHECK(f.records.size()==size);
}

static void invalid_samples_and_time() {
    for (float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),1.0f,0.0f}) {
        Fixture f; auto m=metadata(); f.feed(m); CHECK(!f.records.empty()); f.records.clear();
        std::fill(f.i.begin(),f.i.end(),invalid); std::fill(f.q.begin(),f.q.end(),invalid);
        m.first_sample=10*N; f.feed(m,N);
        CHECK(f.detector.invalid_windows()==1 && f.detector.health()==rf::signal::DetectorHealth::uncertain);
        CHECK(std::isnan(f.detector.noise_floor_db()));
        CHECK(f.records.size()==1 && f.records[0].event.phase==Phase::end && f.records[0].event.end_reason==End::invalid_input);
    }
    for (const int mode:{0,1,2}) {
        Fixture f; auto m=metadata(); m.first_sample=100;
        if (mode==1) m.hw_time_valid=false;
        if (mode==2) m.hw_time_ns=std::numeric_limits<std::int64_t>::max()-1;
        f.feed(m); f.detector.finish(collect,&f.records); CHECK(!f.records.empty());
        for (const auto& r:f.records) {
            CHECK(r.event.first_sample==100+4*N);
            CHECK(r.event.hw_time_valid==(mode==0));
            CHECK(r.event.first_hw_time_ns==(mode==0?5000000:0));
        }
    }
    Fixture narrow; auto m=metadata(); m.bandwidth_hz=1; narrow.feed(m,N);
    CHECK(narrow.detector.invalid_windows()==1 && narrow.records.empty());
}

int main() {
    configuration_validation(); allocation_rollback(); feed_validation(); continuity_and_cache(); invalid_samples_and_time();
    std::puts("test_dsp_boundaries: PASS");
}

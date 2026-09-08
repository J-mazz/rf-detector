#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
#include <cstdint>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#endif
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.dsp;
import rf.signal;
import rf.memory;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c);std::exit(1);}}while(0)
using Event=rf::signal::SpectralCandidate;
static void collect(const Event& e,const rf::signal::CaptureMetadata&,void* ctx) noexcept {
    static_cast<std::vector<Event>*>(ctx)->push_back(e);
}
static rf::signal::CaptureMetadata metadata(){
    auto m=rf::signal::CaptureMetadata::zero();m.sample_rate_sps=30.72e6;m.bandwidth_hz=30e6;
    m.center_frequency_hz=800e6;m.segment_id=1;m.config_id=1;m.hw_time_valid=true;m.hw_time_ns=1000000000;return m;
}
static rf::dsp::DetectorConfig config(std::size_t n=256){
    rf::dsp::DetectorConfig c;c.fft_size=n;c.hop=n/2;c.band_bins=4;c.guard_bands=1;c.reference_bands=2;
    c.warmup_windows=3;c.end_hold_windows=2;c.edge_bins=4;c.dc_bins=1;return c;
}
static void samples(std::vector<float>& i,std::vector<float>& q,std::size_t n){
    std::uint64_t rng=173;const double pi=3.14159265358979323846;
    for(std::size_t k=0;k<i.size();++k){
        auto noise=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (float(int(rng%2001)-1000)/1000)*0.002f;};
        i[k]=noise();q[k]=noise();
        if(k>=3*n && k<10*n){
            i[k]+=0.003f*float(std::cos(2*pi*23*k/n));q[k]+=0.003f*float(std::sin(2*pi*23*k/n));
            i[k]+=0.002f*float(std::cos(-2*pi*37*k/n));q[k]+=0.002f*float(std::sin(-2*pi*37*k/n));
        }
    }
}
static std::vector<Event> run(const std::vector<float>& i,const std::vector<float>& q,std::size_t part,std::size_t n){
    rf::dsp::StreamingDetector d;CHECK(d.initialize(config(n),rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
    std::vector<Event> events;events.reserve(1024);
    for(std::size_t pos=0;pos<i.size();){
        const auto count=std::min(part,i.size()-pos);auto m=metadata();m.first_sample=pos;
        m.hw_time_ns+=static_cast<std::int64_t>(double(pos)*1e9/m.sample_rate_sps);
        rf::signal::PlanarIQBlock b{const_cast<float*>(i.data()+pos),const_cast<float*>(q.data()+pos),count,count,m};
        CHECK(d.feed(b,collect,&events));pos+=count;
    }
    d.finish(collect,&events);
    CHECK(d.windows_processed()==(i.size()-n)/(n/2)+1);
    return events;
}
int main(){
    for(const std::size_t n:{std::size_t{256},std::size_t{8192}}){
        std::vector<float> i(n*14),q(n*14);samples(i,q,n);
        const auto a=run(i,q,i.size(),n),b=run(i,q,127,n);
        CHECK(!a.empty());CHECK(a.size()==b.size());bool pos=false,neg=false;
        for(std::size_t k=0;k<a.size();++k){
            CHECK(a[k].event_id==b[k].event_id);CHECK(a[k].phase==b[k].phase);
            CHECK(a[k].first_sample==b[k].first_sample);CHECK(a[k].end_sample==b[k].end_sample);
            CHECK(a[k].bin_begin==b[k].bin_begin);CHECK(a[k].bin_end==b[k].bin_end);
            CHECK(a[k].segment_id==b[k].segment_id);CHECK(a[k].energy_db==b[k].energy_db);
            CHECK(a[k].noise_floor_db==b[k].noise_floor_db);
            CHECK(a[k].excess_over_background_db==b[k].excess_over_background_db);
            CHECK(a[k].center_offset_hz==b[k].center_offset_hz);CHECK(a[k].bandwidth_hz==b[k].bandwidth_hz);
            CHECK(a[k].duration_s==b[k].duration_s);CHECK(a[k].end_reason==b[k].end_reason);
            CHECK(a[k].first_hw_time_ns==b[k].first_hw_time_ns);CHECK(a[k].hw_time_valid==b[k].hw_time_valid);
            if(a[k].bin_begin<=n/2+23 && a[k].bin_end>n/2+23)pos=true;
            if(a[k].bin_begin<=n/2-37 && a[k].bin_end>n/2-37)neg=true;
        }
        CHECK(pos&&neg);
    }
    rf::dsp::StreamingDetector d;auto cfg=config();cfg.hop=0;
    CHECK(d.initialize(cfg,rf::memory::UnlockedRegionConfig)!=rf::memory::MemoryError::none);
    CHECK(d.initialize(config(),rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
    std::vector<float> i(256,0),q(256,0);std::vector<Event> events;events.reserve(1000);auto m=metadata();
    rf::signal::PlanarIQBlock b{i.data(),q.data(),256,256,m};CHECK(d.feed(b,collect,&events));
    CHECK(d.health()!=rf::signal::DetectorHealth::tracking);CHECK(events.empty());
    for(std::size_t k=0;k<i.size();++k){i[k]=0.001f*float(int(k*13%31)-15);q[k]=0.001f*float(int(k*7%29)-14);}
    for(int k=0;k<12;++k){b.meta.first_sample+=256;CHECK(d.feed(b,collect,&events));}
    CHECK(d.health()==rf::signal::DetectorHealth::tracking);
    auto windows=d.windows_processed();b.count_complex=100;b.meta.first_sample+=256;
    CHECK(d.feed(b,collect,&events));
    b.meta.segment_id=2;b.meta.first_sample=0;b.meta.gap_flags=rf::signal::GapDevice;
    CHECK(d.feed(b,collect,&events));CHECK(d.windows_processed()==windows);CHECK(d.continuity_resets()>0);
    CHECK(d.health()!=rf::signal::DetectorHealth::tracking);
    b.count_complex=257;CHECK(!d.feed(b,collect,&events));
    // A broadband level step must reacquire rather than freeze at the old floor.
    {
        rf::dsp::StreamingDetector noise;CHECK(noise.initialize(config(),rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        std::vector<float> ni(256*40),nq(ni.size());std::uint64_t rng=491;
        for(std::size_t k=0;k<ni.size();++k){
            auto sample=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return float(int(rng%2001)-1000)*0.000001f;};
            const float gain=k<256*16?1.0f:10.0f;ni[k]=sample()*gain;nq[k]=sample()*gain;
        }
        std::vector<Event> found;found.reserve(1000);float before=0;
        for(std::size_t k=0;k<40;++k){
            auto meta=metadata();meta.first_sample=k*256;
            rf::signal::PlanarIQBlock in{ni.data()+k*256,nq.data()+k*256,256,256,meta};CHECK(noise.feed(in,collect,&found));
            if(k==15)before=noise.noise_floor_db();
        }
        CHECK(noise.health()==rf::signal::DetectorHealth::tracking);
        CHECK(noise.noise_floor_db()>before+17 && noise.noise_floor_db()<before+23);
        CHECK(found.empty());
        // Clipped input invalidates confidence rather than becoming a reference.
        std::fill(ni.begin(),ni.begin()+256,1.0f);auto meta=metadata();meta.first_sample=40*256;
        rf::signal::PlanarIQBlock in{ni.data(),nq.data(),256,256,meta};CHECK(noise.feed(in,collect,&found));
        CHECK(noise.health()==rf::signal::DetectorHealth::uncertain);CHECK(noise.invalid_windows()>0);
    }
    // A continuous signal remains an event until explicit finish, even if long.
    {
        const std::size_t n=256;std::vector<float> si(40*n),sq(si.size());samples(si,sq,n);
        for(std::size_t k=3*n;k<si.size();++k){
            si[k]=0.02f*float(std::cos(2*3.14159265358979323846*23*k/n))+0.0001f*float(int(k*17%101)-50);
            sq[k]=0.02f*float(std::sin(2*3.14159265358979323846*23*k/n))+0.0001f*float(int(k*7%103)-51);
        }
        rf::dsp::StreamingDetector persistent;CHECK(persistent.initialize(config(),rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        std::vector<Event> found;found.reserve(1024);auto meta=metadata();
        rf::signal::PlanarIQBlock in{si.data(),sq.data(),si.size(),si.size(),meta};CHECK(persistent.feed(in,collect,&found));
        CHECK(!found.empty()); // a live signal must be observable before it ends
        persistent.finish(collect,&found);bool tail=false;
        for(const auto& e:found)if(e.bin_begin<=151 && e.bin_end>151 && e.end_sample==si.size()){
            CHECK(e.end_reason==rf::signal::EventEnd::finish);CHECK(e.first_sample<10*n);tail=true;
        }
        CHECK(tail);
    }
    {
        auto c=config(8192);rf::dsp::StreamingDetector narrow;
        CHECK(narrow.initialize(c,rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        std::vector<float> ni(8192*6),nq(ni.size());std::uint64_t rng=987;
        for(std::size_t k=0;k<ni.size();++k){auto noise=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return float(int(rng%2001)-1000)*1e-6f;};ni[k]=noise();nq[k]=noise();}
        auto meta=metadata();meta.bandwidth_hz=1e6;std::vector<Event> found;found.reserve(1000);
        rf::signal::PlanarIQBlock in{ni.data(),nq.data(),ni.size(),ni.size(),meta};CHECK(narrow.feed(in,collect,&found));
        CHECK(narrow.health()==rf::signal::DetectorHealth::tracking);
    }
    {
        auto c=config(256);c.band_bins=1;c.range_count=1;c.ranges[0]={150,153};c.update_windows=4;
        rf::dsp::StreamingDetector selected;CHECK(selected.initialize(c,rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
        std::vector<float> ni(256*14),nq(ni.size());samples(ni,nq,256);auto meta=metadata();
        std::vector<Event> found;found.reserve(1000);
        rf::signal::PlanarIQBlock in{ni.data(),nq.data(),ni.size(),ni.size(),meta};CHECK(selected.feed(in,collect,&found));selected.finish(collect,&found);
        bool centered=false,begin=false,update=false,end=false;std::uint64_t id=0;
        for(const auto& e:found){
            CHECK(e.bin_begin>=150 && e.bin_end<=153);
            if(e.bin_begin==151){
                CHECK(std::fabs(e.center_offset_hz-2760000.0f)<1.0f);centered=true;
                if(!id)id=e.event_id;
                CHECK(id==e.event_id);
                begin|=e.phase==rf::signal::EventPhase::begin;update|=e.phase==rf::signal::EventPhase::update;end|=e.phase==rf::signal::EventPhase::end;
            }
        }
        CHECK(centered && begin && update && end);
    }
    // Exact dB boundaries are inclusive, including the lower release boundary.
    {
        constexpr std::size_t n=256,frames=12,target_bin=151;
        auto detect=[&](const float (&amplitude)[frames],float threshold,float release){
            auto c=config(n);c.hop=n;c.band_bins=1;c.range_count=1;c.ranges[0]={target_bin,target_bin+1};
            c.threshold_db=threshold;c.release_db=release;
            rf::dsp::StreamingDetector detector;
            CHECK(detector.initialize(c,rf::memory::UnlockedRegionConfig)==rf::memory::MemoryError::none);
            std::vector<float> hi(n*frames),hq(hi.size());
            for(std::size_t frame=0;frame<frames;++frame){
                std::uint64_t rng=173;
                for(std::size_t x=0;x<n;++x){
                    auto noise=[&]{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (float(int(rng%2001)-1000)/1000)*0.002f;};
                    const auto k=frame*n+x;
                    hi[k]=noise()+amplitude[frame]*float(std::cos(2*3.14159265358979323846*23*x/n));
                    hq[k]=noise()+amplitude[frame]*float(std::sin(2*3.14159265358979323846*23*x/n));
                }
            }
            std::vector<Event> found;found.reserve(4);auto meta=metadata();
            rf::signal::PlanarIQBlock in{hi.data(),hq.data(),hi.size(),hi.size(),meta};
            CHECK(detector.feed(in,collect,&found));detector.finish(collect,&found);return found;
        };
        const float high[frames]={0,0,0,0,0.003f,0.003f,0.003f,0.003f,0,0,0,0};
        const float low[frames]={0,0,0,0,0.0018f,0.0018f,0.0018f,0.0018f,0,0,0,0};
        const auto high_probe=detect(high,1.0f,0.5f),low_probe=detect(low,1.0f,0.5f);
        CHECK(!high_probe.empty() && !low_probe.empty());
        const float entry=high_probe.front().excess_over_background_db;
        const float release=low_probe.front().excess_over_background_db;
        CHECK(entry>release);
        CHECK(!detect(high,entry,release).empty());
        CHECK(detect(high,std::nextafter(entry,entry+1.0f),release).empty());
        const float amplitude[frames]={0,0,0,0,0.003f,0.0018f,0.0018f,0.0018f,0,0,0,0};
        const auto found=detect(amplitude,entry,release);
        CHECK(found.size()==2);
        CHECK(found[0].phase==rf::signal::EventPhase::begin);
        CHECK(found[0].first_sample==4*n);
        CHECK(found[0].excess_over_background_db==entry);
        CHECK(found[1].phase==rf::signal::EventPhase::end);
        CHECK(found[1].event_id==found[0].event_id);
        CHECK(found[1].end_sample==8*n);
        CHECK(found[1].end_reason==rf::signal::EventEnd::quiet);
    }
    std::puts("test_dsp: PASS");
}

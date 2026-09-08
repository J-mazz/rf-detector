// Streaming periodic-Hann STFT with guarded spectral references and per-band events.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#endif
export module rf.dsp;
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.memory;
import rf.signal;
import rf.fft;
export namespace rf::dsp {
struct BinRange { std::size_t begin,end; };
struct DetectorConfig {
    std::size_t fft_size=8192,hop=4096,band_bins=4;
    std::size_t guard_bands=2,reference_bands=4;
    std::size_t warmup_windows=8,end_hold_windows=2,update_windows=128;
    // Optional detection masks. Bands overlapping a range are selected;
    // background references still use all usable spectrum. Zero ranges selects all.
    BinRange ranges[16]{};std::size_t range_count=0;
    std::size_t edge_bins=32,dc_bins=2;
    float threshold_db=12.0f,release_db=6.0f;
    float max_clipped_fraction=0.01f,floor_shift_db=10.0f;
};
using EventCallback=void(*)(const rf::signal::SpectralCandidate&,const rf::signal::CaptureMetadata&,void*) noexcept;
inline float power_db(double power) noexcept {return static_cast<float>(10.0*std::log10(std::max(power,1e-30)));}
class StreamingDetector {
    struct Band {
        double power,noise;
        std::uint64_t first,last,id;
        float peak_energy,peak_noise,peak_excess;
        std::uint32_t quiet,updates;
        bool valid,selected,active;
    };
public:
    [[nodiscard]] rf::memory::MemoryError initialize(DetectorConfig cfg={},rf::memory::RegionConfig region=rf::memory::DefaultRegionConfig) noexcept {
        using E=rf::memory::MemoryError;
        if(ready_)return E::already_initialized;
        const auto n=cfg.fft_size;
        if(n<256 || n>65536 || !rf::core::is_power_of_two(n) || !cfg.hop || cfg.hop>n ||
           !cfg.band_bins || n%cfg.band_bins || !cfg.reference_bands || cfg.reference_bands>64 ||
           cfg.guard_bands>64 || !cfg.warmup_windows || cfg.warmup_windows>1000000 ||
           !cfg.end_hold_windows || cfg.end_hold_windows>1000000 ||
           !cfg.update_windows || cfg.update_windows>1000000 || cfg.range_count>16 ||
           cfg.edge_bins>=n/4 || cfg.dc_bins>=n/4 ||
           !std::isfinite(cfg.threshold_db) || !std::isfinite(cfg.release_db) ||
           cfg.release_db<=0 || cfg.threshold_db<=cfg.release_db ||
           !std::isfinite(cfg.max_clipped_fraction) || cfg.max_clipped_fraction<0 || cfg.max_clipped_fraction>=1 ||
           !std::isfinite(cfg.floor_shift_db) || cfg.floor_shift_db<=0 ||
           n/cfg.band_bins<=4*cfg.reference_bands+2*cfg.guard_bands+4)return E::invalid_size;
        for(std::size_t k=0;k<cfg.range_count;++k)
            if(cfg.ranges[k].begin>=cfg.ranges[k].end || cfg.ranges[k].end>n)return E::invalid_size;
        cfg_=cfg;band_count_=n/cfg.band_bins;fft_mask_=n-1;
        auto fail=[&](E e){reset();return e;};
        if(auto e=fft_.initialize(n,region);e!=E::none)return fail(e);
        if(auto e=samples_.map(2*n,region);e!=E::none)return fail(e);
        if(auto e=window_.map(n,region);e!=E::none)return fail(e);
        if(auto e=bands_.map(band_count_,region);e!=E::none)return fail(e);
        if(auto e=prefix_.map(band_count_+1,region);e!=E::none)return fail(e);
        if(auto e=counts_.map(band_count_+1,region);e!=E::none)return fail(e);
        window_energy_=0;
        for(std::size_t k=0;k<n;++k){
            const float w=static_cast<float>(0.5-0.5*std::cos(2.0*3.14159265358979323846*static_cast<double>(k)/n));
            window_.data()[k]=w;window_energy_+=static_cast<double>(w)*w;
        }
        normalization_=1.0/(static_cast<double>(n)*window_energy_);
        constexpr double prefilter_margin_db=0.25;
        entry_prefilter_ratio_=std::pow(10.0,(static_cast<double>(cfg.threshold_db)-prefilter_margin_db)/10.0);
        release_prefilter_ratio_=std::pow(10.0,(static_cast<double>(cfg.release_db)-prefilter_margin_db)/10.0);
        for(std::size_t k=0;k<band_count_;++k)bands_.data()[k]=Band{};
        ready_=true;return E::none;
    }
    // Control plane only. Never call while a feed or callback is in flight.
    void reset() noexcept {
        fft_.reset();samples_.unmap();window_.unmap();bands_.unmap();prefix_.unmap();counts_.unmap();
        ready_=false;have_segment_=false;write_=0;warm_=0;floor_seeded_=false;
        health_=rf::signal::DetectorHealth::uncalibrated;
    }
    [[nodiscard]] bool feed(const rf::signal::PlanarIQBlock& b,EventCallback callback,void* context) noexcept {
        if(!ready_ || !callback || b.count_complex>b.capacity_complex ||
           (b.count_complex && (!b.i || !b.q)) || !std::isfinite(b.meta.sample_rate_sps) ||
           b.meta.sample_rate_sps<=0 || b.meta.sample_rate_sps>1e9 ||
           !std::isfinite(b.meta.center_frequency_hz) || !std::isfinite(b.meta.bandwidth_hz) ||
           b.meta.bandwidth_hz<=0 || b.meta.bandwidth_hz>b.meta.sample_rate_sps || !std::isfinite(b.meta.gain_db) ||
           b.meta.first_sample>std::numeric_limits<std::uint64_t>::max()-b.count_complex)return false;
        if(!b.count_complex)return true;
        const bool changed=have_segment_ && (b.meta.segment_id!=anchor_.segment_id || b.meta.config_id!=anchor_.config_id ||
            b.meta.first_sample!=expected_sample_ || b.meta.sample_rate_sps!=anchor_.sample_rate_sps ||
            b.meta.center_frequency_hz!=anchor_.center_frequency_hz || b.meta.bandwidth_hz!=anchor_.bandwidth_hz ||
            b.meta.gain_db!=anchor_.gain_db || b.meta.gap_flags!=rf::signal::GapNone);
        if(changed){close_all(rf::signal::EventEnd::gap,callback,context);++resets_;have_segment_=false;}
        if(!have_segment_){
            anchor_=b.meta;expected_sample_=b.meta.first_sample;write_=0;until_window_=cfg_.fft_size;
            warm_=0;floor_seeded_=false;health_=rf::signal::DetectorHealth::uncalibrated;have_segment_=true;
            configure_segment();
        }
        for(std::size_t k=0;k<b.count_complex;++k){
            samples_.data()[write_]={b.i[k],b.q[k]};write_=(write_+1)&fft_mask_;++expected_sample_;
            if(--until_window_==0){analyze(expected_sample_-cfg_.fft_size,callback,context);until_window_=cfg_.hop;}
        }
        return true;
    }
    void finish(EventCallback callback,void* context) noexcept {
        if(ready_ && callback)close_all(rf::signal::EventEnd::finish,callback,context);
        have_segment_=false;write_=0;health_=rf::signal::DetectorHealth::uncalibrated;
    }
    [[nodiscard]] std::uint64_t windows_processed() const noexcept{return windows_;}
    [[nodiscard]] std::uint64_t invalid_windows() const noexcept{return invalid_;}
    [[nodiscard]] std::uint64_t continuity_resets() const noexcept{return resets_;}
    [[nodiscard]] rf::signal::DetectorHealth health() const noexcept{return health_;}
    [[nodiscard]] float noise_floor_db() const noexcept{return floor_seeded_?floor_db_:std::numeric_limits<float>::quiet_NaN();}
private:
    static void sort_references(double (&values)[4],std::size_t count) noexcept {
        for(std::size_t i=1;i<count && i<4;++i){
            const double value=values[i];std::size_t j=i;
            while(j && value<values[j-1]){values[j]=values[j-1];--j;}
            values[j]=value;
        }
    }
    void configure_segment() noexcept {
        const auto n=cfg_.fft_size;counts_.data()[0]=0;
        for(std::size_t b=0;b<band_count_;++b){
            auto& s=bands_.data()[b];const auto lo=b*cfg_.band_bins,hi=lo+cfg_.band_bins;
            const double f0=(static_cast<double>(lo)-n/2)*anchor_.sample_rate_sps/n;
            const double f1=(static_cast<double>(hi)-n/2)*anchor_.sample_rate_sps/n;
            s.valid=lo>=cfg_.edge_bins && hi<=n-cfg_.edge_bins &&
                !(lo<n/2+cfg_.dc_bins && hi>n/2-cfg_.dc_bins) &&
                f0>=-anchor_.bandwidth_hz/2 && f1<=anchor_.bandwidth_hz/2;
            s.selected=cfg_.range_count==0;
            for(std::size_t j=0;j<cfg_.range_count;++j)
                if(lo<cfg_.ranges[j].end && hi>cfg_.ranges[j].begin)s.selected=true;
            counts_.data()[b+1]=counts_.data()[b]+(s.valid?1:0);
        }
    }
    void publish(std::size_t b,rf::signal::EventEnd reason,rf::signal::EventPhase phase,EventCallback callback,void* context) noexcept {
        auto& s=bands_.data()[b];if(!s.active)return;
        rf::signal::SpectralCandidate e{};e.event_id=s.id;e.phase=phase;e.bin_begin=static_cast<std::uint32_t>(b*cfg_.band_bins);
        e.bin_end=static_cast<std::uint32_t>((b+1)*cfg_.band_bins);e.first_sample=s.first;e.end_sample=s.last;
        e.segment_id=anchor_.segment_id;e.energy_db=s.peak_energy;e.noise_floor_db=s.peak_noise;
        e.excess_over_background_db=s.peak_excess;
        e.center_offset_hz=static_cast<float>((static_cast<double>(e.bin_begin+e.bin_end-1)/2-cfg_.fft_size/2)*anchor_.sample_rate_sps/cfg_.fft_size);
        e.bandwidth_hz=static_cast<float>(cfg_.band_bins*anchor_.sample_rate_sps/cfg_.fft_size);
        e.duration_s=static_cast<float>((s.last-s.first)/anchor_.sample_rate_sps);e.end_reason=reason;
        if(anchor_.hw_time_valid){
            const long double t=static_cast<long double>(anchor_.hw_time_ns)+static_cast<long double>(s.first-anchor_.first_sample)*1e9L/anchor_.sample_rate_sps;
            if(t>=std::numeric_limits<std::int64_t>::min() && t<=std::numeric_limits<std::int64_t>::max()){
                e.first_hw_time_ns=static_cast<std::int64_t>(t);e.hw_time_valid=true;
            }
        }
        callback(e,anchor_,context);
    }
    void close(std::size_t b,rf::signal::EventEnd reason,EventCallback callback,void* context) noexcept {
        publish(b,reason,rf::signal::EventPhase::end,callback,context);
        auto& s=bands_.data()[b];s.active=false;s.quiet=0;
    }
    void close_all(rf::signal::EventEnd reason,EventCallback callback,void* context) noexcept {
        for(std::size_t b=0;b<band_count_;++b)close(b,reason,callback,context);
    }
    void uncertain(EventCallback callback,void* context) noexcept {
        close_all(rf::signal::EventEnd::invalid_input,callback,context);
        health_=rf::signal::DetectorHealth::uncertain;warm_=0;floor_seeded_=false;
    }
    void analyze(std::uint64_t first,EventCallback callback,void* context) noexcept {
        ++windows_;const auto n=cfg_.fft_size;auto* transformed=samples_.data()+n;
        double input_energy=0;std::size_t clipped=0;
        for(std::size_t k=0;k<n;++k){
            const auto v=samples_.data()[(write_+k)&fft_mask_];
            if(!std::isfinite(v.re) || !std::isfinite(v.im)){++invalid_;uncertain(callback,context);return;}
            if(std::fabs(v.re)>=0.999f || std::fabs(v.im)>=0.999f)++clipped;
            input_energy+=static_cast<double>(v.re)*v.re+static_cast<double>(v.im)*v.im;
            transformed[k]={v.re*window_.data()[k],v.im*window_.data()[k]};
        }
        if(input_energy<=1e-20 || static_cast<double>(clipped)/n>cfg_.max_clipped_fraction){++invalid_;uncertain(callback,context);return;}
        fft_.execute(transformed);
        prefix_.data()[0]=0;
        for(std::size_t b=0;b<band_count_;++b){
            auto& s=bands_.data()[b];s.power=0;
            const auto lo=b*cfg_.band_bins,hi=lo+cfg_.band_bins;
            for(auto k=lo;k<hi;++k){const auto v=transformed[(k+n/2)&fft_mask_];s.power+=(static_cast<double>(v.re)*v.re+static_cast<double>(v.im)*v.im)*normalization_;}
            prefix_.data()[b+1]=prefix_.data()[b]+(s.valid?s.power:0);
            s.noise=0;
        }
        // Median of up to four guarded reference-block means. Prefix sums keep
        // this O(number of bands), and reduce contamination by isolated emitters.
        double floor_samples[32];std::size_t floor_count=0;
        const auto stride=std::max<std::size_t>(1,counts_.data()[band_count_]/32);
        std::size_t usable=0;
        for(std::size_t b=0;b<band_count_;++b){
            auto& s=bands_.data()[b];if(!s.valid)continue;
            double refs[4];std::size_t used=0;
            const auto r=cfg_.reference_bands,g=cfg_.guard_bands;
            auto reference=[&](std::size_t lo,std::size_t hi){
                const auto count=counts_.data()[hi]-counts_.data()[lo];
                if(count>=std::max<std::size_t>(1,r/2) && used<4)refs[used++]=(prefix_.data()[hi]-prefix_.data()[lo])/count;
            };
            if(b>=g+2*r){reference(b-g-2*r,b-g-r);reference(b-g-r,b-g);}
            if(b+1+g+2*r<=band_count_){reference(b+1+g,b+1+g+r);reference(b+1+g+r,b+1+g+2*r);}
            if(used<2)continue;
            sort_references(refs,used);s.noise=(refs[(used-1)/2]+refs[used/2])*0.5;
            if(usable++%stride==0 && floor_count<32 && s.noise>1e-30)floor_samples[floor_count++]=s.noise;
        }
        if(floor_count<4){++invalid_;uncertain(callback,context);return;}
        std::sort(floor_samples,floor_samples+floor_count);
        const float current_floor=power_db(floor_samples[floor_count/2]);
        if(floor_seeded_ && std::fabs(current_floor-floor_db_)>cfg_.floor_shift_db){uncertain(callback,context);}
        floor_db_=floor_seeded_?0.9f*floor_db_+0.1f*current_floor:current_floor;floor_seeded_=true;
        if(warm_<cfg_.warmup_windows){++warm_;if(warm_>=cfg_.warmup_windows)health_=rf::signal::DetectorHealth::tracking;return;}
        health_=rf::signal::DetectorHealth::tracking;
        for(std::size_t b=0;b<band_count_;++b){
            auto& s=bands_.data()[b];
            float excess=0;bool above=false;
            if(s.selected && s.valid && s.noise>1e-30){
                const bool active=s.active;
                const double prefilter=active?release_prefilter_ratio_:entry_prefilter_ratio_;
                if(!(s.power<s.noise*prefilter)){
                    excess=power_db(s.power)-power_db(s.noise);
                    above=excess>=(active?cfg_.release_db:cfg_.threshold_db);
                }
            }
            if(above){
                const bool starting=!s.active;
                if(starting){s.active=true;s.first=first;s.id=next_event_id_++;s.updates=0;s.peak_excess=-std::numeric_limits<float>::infinity();}
                s.last=first+n;s.quiet=0;
                if(excess>s.peak_excess){s.peak_excess=excess;s.peak_energy=power_db(s.power);s.peak_noise=power_db(s.noise);}
                if(starting)publish(b,rf::signal::EventEnd::quiet,rf::signal::EventPhase::begin,callback,context);
                else if(++s.updates>=cfg_.update_windows){s.updates=0;publish(b,rf::signal::EventEnd::quiet,rf::signal::EventPhase::update,callback,context);}
            }else if(s.active && ++s.quiet>=cfg_.end_hold_windows)close(b,rf::signal::EventEnd::quiet,callback,context);
        }
    }
    DetectorConfig cfg_{};rf::fft::Radix2FFT fft_;
    rf::memory::PinnedRegion<rf::fft::Complex> samples_;
    rf::memory::PinnedRegion<float> window_;
    rf::memory::PinnedRegion<Band> bands_;
    rf::memory::PinnedRegion<double> prefix_;
    rf::memory::PinnedRegion<std::uint32_t> counts_;
    std::size_t band_count_{0},fft_mask_{0},write_{0},until_window_{0},warm_{0};
    double window_energy_{0},normalization_{0},entry_prefilter_ratio_{0},release_prefilter_ratio_{0};
    bool ready_{false},have_segment_{false},floor_seeded_{false};
    float floor_db_{0};rf::signal::DetectorHealth health_{rf::signal::DetectorHealth::uncalibrated};
    std::uint64_t next_event_id_{1},expected_sample_{0},windows_{0},invalid_{0},resets_{0};rf::signal::CaptureMetadata anchor_{};
};
}

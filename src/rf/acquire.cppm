// rf.acquire: synchronous SC16 sources. Configuration/allocation precede workers.
module;
#include <version>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#endif
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
export module rf.acquire;
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
export namespace rf::acquire {
enum class ReadStatus : std::uint8_t { ok, timeout, overflow, failure, stopped };
struct ReadResult {
    std::size_t complex_samples;
    ReadStatus status;
    std::int64_t hw_time_ns;
    bool hw_time_valid;
};
struct ReceiverConfig {
    double center_frequency_hz;
    double sample_rate_sps;
    double bandwidth_hz;
    float gain_db;
    std::uint32_t receiver_id;
    float sample_scale = 1.0f/32768.0f;
    std::uint64_t config_id = 1;
};
inline bool valid_config(const ReceiverConfig& c) noexcept {
    return std::isfinite(c.center_frequency_hz) && c.center_frequency_hz>0 &&
        std::isfinite(c.sample_rate_sps) && c.sample_rate_sps>0 && c.sample_rate_sps<=1e9 &&
        std::isfinite(c.bandwidth_hz) && c.bandwidth_hz>0 && c.bandwidth_hz<=c.sample_rate_sps &&
        std::isfinite(c.gain_db) && std::isfinite(c.sample_scale) && c.sample_scale>0 && c.sample_scale<=1;
}
class SDRSource {
public:
    virtual ~SDRSource() noexcept;
    virtual bool configure(const ReceiverConfig&) noexcept=0;
    virtual bool start() noexcept=0;
    virtual void stop() noexcept=0;
    // Implementations must obey capacity and return only fully written I/Q pairs.
    virtual ReadResult read_into(std::int16_t*,std::size_t,std::uint32_t) noexcept=0;
    virtual const ReceiverConfig& config() const noexcept=0;
    // Replay can wait for capacity; live receivers must keep draining.
    virtual bool lossless() const noexcept { return false; }
};
class MockSource final : public SDRSource {
public:
    explicit MockSource(bool realtime=false,std::uint64_t seed=0x9E3779B97F4A7C15ull) noexcept
        : realtime_(realtime),seed_(seed?seed:1),rng_(seed_) {}
    bool configure(const ReceiverConfig& c) noexcept override {
        if(running_ || !valid_config(c)) return false;
        cfg_=c; configured_=true;
        const double w=2.0*3.14159265358979323846*0.05;
        rot_re_=static_cast<float>(std::cos(w));rot_im_=static_cast<float>(std::sin(w));
        return true;
    }
    bool start() noexcept override {
        if(!configured_ || running_) return false;
        sample_clock_=0;reads_=0;rng_=seed_;ph_re_=1;ph_im_=0;
        start_ns_=rf::core::monotonic_now_ns();running_=true;return true;
    }
    void stop() noexcept override {running_=false;}
    ReadResult read_into(std::int16_t* dst,std::size_t capacity,std::uint32_t timeout_us) noexcept override {
        if(!running_) return {0,ReadStatus::stopped,0,false};
        if(!capacity || !dst) return {0,ReadStatus::failure,0,false};
        const std::size_t requested=(++reads_%5==0 && capacity>1)?std::max<std::size_t>(1,capacity*3/5):capacity;
        const auto room=std::numeric_limits<std::uint64_t>::max()-sample_clock_;
        std::size_t count=std::min<std::uint64_t>(requested,room);
        if(!count)return {0,ReadStatus::failure,0,false};
        if(realtime_){
            const auto now=rf::core::monotonic_now_ns();
            const auto timeout_ns=static_cast<std::uint64_t>(timeout_us)*1000u;
            const auto deadline=timeout_ns>std::numeric_limits<std::uint64_t>::max()-now?
                std::numeric_limits<std::uint64_t>::max():now+timeout_ns;
            const auto elapsed=deadline>start_ns_?deadline-start_ns_:0;
            const long double due=static_cast<long double>(elapsed)*static_cast<long double>(cfg_.sample_rate_sps)/1'000'000'000.0L;
            const long double available=due-static_cast<long double>(sample_clock_);
            if(available<1.0L){
                const auto before_sleep=rf::core::monotonic_now_ns();
                if(deadline>before_sleep)rf::core::sleep_ns(deadline-before_sleep);
                return {0,ReadStatus::timeout,0,false};
            }
            if(available<static_cast<long double>(count))count=static_cast<std::size_t>(available);
            const auto target_clock=sample_clock_+static_cast<std::uint64_t>(count);
            const long double target_elapsed_ld=std::ceil(static_cast<long double>(target_clock)*1'000'000'000.0L/
                static_cast<long double>(cfg_.sample_rate_sps));
            const auto deadline_elapsed=deadline>start_ns_?deadline-start_ns_:0;
            const auto target_elapsed=target_elapsed_ld>=static_cast<long double>(deadline_elapsed)?
                deadline_elapsed:static_cast<std::uint64_t>(target_elapsed_ld);
            const auto wake=start_ns_+target_elapsed;
            const auto before_sleep=rf::core::monotonic_now_ns();
            if(wake>before_sleep)rf::core::sleep_ns(wake-before_sleep);
        }
        const long double hw_ld=static_cast<long double>(sample_clock_)*1'000'000'000.0L/
            static_cast<long double>(cfg_.sample_rate_sps);
        // 2^63 is exactly representable; values below it are precisely the
        // nonnegative range that may be converted to int64_t.
        const bool hw_valid=hw_ld<9'223'372'036'854'775'808.0L;
        const auto hw=hw_valid?static_cast<std::int64_t>(hw_ld):0;
        for(std::size_t k=0;k<count;++k,++sample_clock_){
            int i=noise(),q=noise();
            const auto phase=sample_clock_%(7*131072ull);
            if(phase>=2*131072ull && phase<3*131072ull){i+=static_cast<int>(2000*ph_re_);q+=static_cast<int>(2000*ph_im_);}
            dst[2*k]=static_cast<std::int16_t>(i);dst[2*k+1]=static_cast<std::int16_t>(q);
            const float re=ph_re_*rot_re_-ph_im_*rot_im_;
            ph_im_=ph_re_*rot_im_+ph_im_*rot_re_;ph_re_=re;
            if((sample_clock_&1023u)==0){const float m=std::sqrt(ph_re_*ph_re_+ph_im_*ph_im_);ph_re_/=m;ph_im_/=m;}
        }
        return {count,ReadStatus::ok,hw,hw_valid};
    }
    const ReceiverConfig& config() const noexcept override {return cfg_;}
private:
    int noise() noexcept {rng_^=rng_<<13;rng_^=rng_>>7;rng_^=rng_<<17;return static_cast<int>(rng_%401u)-200;}
    bool realtime_,running_{false},configured_{false};
    std::uint64_t seed_,rng_,sample_clock_{0},reads_{0},start_ns_{0};
    float rot_re_{1},rot_im_{0},ph_re_{1},ph_im_{0};
    ReceiverConfig cfg_{};
};
// Raw little-endian interleaved signed-16 IQ; caller supplies RF metadata.
// Regular files only. The descriptor is opened and validated before worker start.
class ReplaySource final : public SDRSource {
public:
    explicit ReplaySource(const char* path,std::size_t max_read=131072) noexcept
        : path_(path),max_read_(max_read) {}
    ~ReplaySource() noexcept override {stop();}
    bool configure(const ReceiverConfig& c) noexcept override {
        if(fd_>=0 || !valid_config(c) || !max_read_)return false;
        cfg_=c;configured_=true;return true;
    }
    bool start() noexcept override {
        if(!configured_ || fd_>=0)return false;
        fd_=::open(path_,O_RDONLY|O_CLOEXEC);
        if(fd_<0)return false;
        struct stat st{};
        if(::fstat(fd_,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size%4!=0){stop();return false;}
        return true;
    }
    void stop() noexcept override {if(fd_>=0){::close(fd_);fd_=-1;}}
    bool lossless() const noexcept override {return true;}
    ReadResult read_into(std::int16_t* dst,std::size_t capacity,std::uint32_t) noexcept override {
        if(fd_<0)return {0,ReadStatus::stopped,0,false};
        const auto count=std::min(capacity,max_read_);
        if(!count || !dst || count>static_cast<std::size_t>(0x7fffffff)/4)return {0,ReadStatus::failure,0,false};
        ssize_t n;do{n=::read(fd_,dst,count*4);}while(n<0 && errno==EINTR);
        if(n<0 || n%4!=0)return {0,ReadStatus::failure,0,false};
        if(n==0)return {0,ReadStatus::stopped,0,false};
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        for(ssize_t j=0;j<n/2;++j)dst[j]=static_cast<std::int16_t>(__builtin_bswap16(static_cast<std::uint16_t>(dst[j])));
#endif
        return {static_cast<std::size_t>(n)/4,ReadStatus::ok,0,false};
    }
    const ReceiverConfig& config() const noexcept override {return cfg_;}
private:
    const char* path_;std::size_t max_read_;int fd_{-1};bool configured_{false};ReceiverConfig cfg_{};
};
SDRSource::~SDRSource() noexcept=default;
}

// rf.soapy: optional single-channel SoapySDR C API receiver adapter.
module;
#include <version>
#ifdef RF_WITH_SOAPY
#ifndef RF_IMPORT_STD
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#endif
#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.h>
#endif

export module rf.soapy;
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.acquire;

#ifdef RF_WITH_SOAPY
export namespace rf::acquire {

class SoapySource final : public SDRSource {
public:
    explicit SoapySource(const char* device_args) noexcept {
        if (!device_args) device_args="";
        const auto length=std::strlen(device_args);
        if (length>=sizeof(device_args_)) {
            args_valid_=false;
            set_error("SoapySDR device arguments exceed adapter limit");
            return;
        }
        std::memcpy(device_args_,device_args,length+1);
    }
    ~SoapySource() noexcept override {
        stop();
        if (device_) {
            const int rc=SoapySDRDevice_unmake(device_);
            if (rc!=0) set_api_error("SoapySDR device release failed",rc);
            device_=nullptr;
        }
        configured_=false;
    }

    SoapySource(const SoapySource&)=delete;
    SoapySource& operator=(const SoapySource&)=delete;

    bool configure(const ReceiverConfig& requested) noexcept override {
        if (running_) return fail("cannot configure a running SoapySDR source");
        if (!args_valid_) return false;
        if (!valid_config(requested)) return fail("invalid requested SoapySDR receiver configuration");
        if (device_) return fail("SoapySDR source is already configured");
        clear_error();
        cfg_={};configured_=false;

        device_=SoapySDRDevice_makeStrArgs(device_args_);
        if (!device_) {
            set_api_error("SoapySDR device creation failed",SoapySDRDevice_lastStatus());
            return false;
        }
        const auto rollback=[this]() noexcept { preserve_error_and_unmake();return false; };

        const auto channels=SoapySDRDevice_getNumChannels(device_,SOAPY_SDR_RX);
        if (const int status=SoapySDRDevice_lastStatus();status!=0) {
            set_api_error("SoapySDR RX channel query failed",status);return rollback();
        }
        if (channels==0) {set_error("SoapySDR device has no RX channel 0");return rollback();}

        if (const int rc=SoapySDRDevice_setSampleRate(device_,SOAPY_SDR_RX,0,requested.sample_rate_sps);rc!=0) {
            set_api_error("SoapySDR sample-rate configuration failed",rc);return rollback();
        }
        if (const int rc=SoapySDRDevice_setBandwidth(device_,SOAPY_SDR_RX,0,requested.bandwidth_hz);rc!=0) {
            set_api_error("SoapySDR bandwidth configuration failed",rc);return rollback();
        }
        if (const int rc=SoapySDRDevice_setFrequency(device_,SOAPY_SDR_RX,0,requested.center_frequency_hz,nullptr);rc!=0) {
            set_api_error("SoapySDR center-frequency configuration failed",rc);return rollback();
        }
        if (const int rc=SoapySDRDevice_setGain(device_,SOAPY_SDR_RX,0,requested.gain_db);rc!=0) {
            set_api_error("SoapySDR gain configuration failed",rc);return rollback();
        }

        cfg_=requested;
        cfg_.sample_rate_sps=SoapySDRDevice_getSampleRate(device_,SOAPY_SDR_RX,0);
        if (!getter_ok("SoapySDR sample-rate readback failed")) return rollback();
        cfg_.bandwidth_hz=SoapySDRDevice_getBandwidth(device_,SOAPY_SDR_RX,0);
        if (!getter_ok("SoapySDR bandwidth readback failed")) return rollback();
        cfg_.center_frequency_hz=SoapySDRDevice_getFrequency(device_,SOAPY_SDR_RX,0);
        if (!getter_ok("SoapySDR center-frequency readback failed")) return rollback();
        cfg_.gain_db=static_cast<float>(SoapySDRDevice_getGain(device_,SOAPY_SDR_RX,0));
        if (!getter_ok("SoapySDR gain readback failed")) return rollback();

        double full_scale=0;
        char* native_format=SoapySDRDevice_getNativeStreamFormat(device_,SOAPY_SDR_RX,0,&full_scale);
        const int format_status=SoapySDRDevice_lastStatus();
        if (format_status!=0 || !native_format) {
            set_api_error("SoapySDR native stream-format query failed",format_status);
            if (native_format) SoapySDR_free(native_format);
            return rollback();
        }
        const bool native_cs16=std::strcmp(native_format,SOAPY_SDR_CS16)==0;
        if (!native_cs16) {
            set_error("unsupported native SoapySDR stream format",native_format);
            append_error("; conversion sample scale is ambiguous");
            SoapySDR_free(native_format);
            return rollback();
        }
        SoapySDR_free(native_format);

        if (!std::isfinite(full_scale) || full_scale<1.0) {
            set_error("invalid SoapySDR CS16 full-scale readback");return rollback();
        }
        cfg_.sample_scale=static_cast<float>(1.0/full_scale);
        if (!valid_config(cfg_)) {
            set_error("invalid actual SoapySDR receiver configuration readback");return rollback();
        }
        configured_=true;
        clear_error();
        return true;
    }

    bool start() noexcept override {
        if (!configured_ || !device_) return fail("SoapySDR source is not configured");
        if (running_ || stream_) return fail("SoapySDR source is already started");
        clear_error();
        const std::size_t channel=0;
        stream_=SoapySDRDevice_setupStream(device_,SOAPY_SDR_RX,SOAPY_SDR_CS16,&channel,1,nullptr);
        if (!stream_) {
            set_api_error("SoapySDR stream setup failed",SoapySDRDevice_lastStatus());
            return false;
        }
        if (const int rc=SoapySDRDevice_activateStream(device_,stream_,0,0,0);rc!=0) {
            set_api_error("SoapySDR stream activation failed",rc);
            char saved[ErrorCapacity];copy(saved,sizeof(saved),error_);
            (void)SoapySDRDevice_closeStream(device_,stream_);stream_=nullptr;
            copy(error_,sizeof(error_),saved);
            return false;
        }
        running_=true;
        return true;
    }

    void stop() noexcept override {
        if (!stream_) {running_=false;return;}
        bool have_cleanup_error=false;
        if (running_) {
            if (const int rc=SoapySDRDevice_deactivateStream(device_,stream_,0,0);rc!=0) {
                set_api_error("SoapySDR stream deactivation failed",rc);have_cleanup_error=true;
            }
        }
        running_=false;
        if (const int rc=SoapySDRDevice_closeStream(device_,stream_);rc!=0 && !have_cleanup_error)
            set_api_error("SoapySDR stream close failed",rc);
        stream_=nullptr;
    }

    ReadResult read_into(std::int16_t* dst,std::size_t capacity,std::uint32_t timeout_us) noexcept override {
        if (!running_ || !stream_) return {0,ReadStatus::stopped,0,false};
        if (!dst || capacity==0) {
            set_error("invalid SoapySDR destination buffer");return {0,ReadStatus::failure,0,false};
        }
        void* buffers[1]={dst};int flags=0;long long time_ns=0;
        const auto max_timeout=static_cast<std::uint64_t>(std::numeric_limits<long>::max());
        const long timeout=static_cast<long>(static_cast<std::uint64_t>(timeout_us)>max_timeout?max_timeout:timeout_us);
        const int count=SoapySDRDevice_readStream(device_,stream_,buffers,capacity,&flags,&time_ns,timeout);
        if (count>0) {
            if (static_cast<std::size_t>(count)>capacity) {
                set_error("SoapySDR driver returned more samples than requested");return {0,ReadStatus::failure,0,false};
            }
            const bool timed=(flags&SOAPY_SDR_HAS_TIME)!=0;
            const auto status=(flags&SOAPY_SDR_END_ABRUPT)!=0?ReadStatus::overflow:ReadStatus::ok;
            return {static_cast<std::size_t>(count),status,static_cast<std::int64_t>(time_ns),timed};
        }
        if (count==0 || count==SOAPY_SDR_TIMEOUT) return {0,ReadStatus::timeout,0,false};
        if (count==SOAPY_SDR_OVERFLOW) return {0,ReadStatus::overflow,0,false};
        set_api_error("SoapySDR stream read failed",count);
        return {0,ReadStatus::failure,0,false};
    }

    const ReceiverConfig& config() const noexcept override {return cfg_;}
    const char* last_error() const noexcept {return error_;}

private:
    static constexpr std::size_t ArgsCapacity=1024;
    static constexpr std::size_t ErrorCapacity=512;

    static void copy(char* dst,std::size_t capacity,const char* src) noexcept {
        if (!capacity) return;
        if (!src) src="";
        const auto length=std::strlen(src);
        const auto count=length<capacity-1?length:capacity-1;
        if (count) std::memcpy(dst,src,count);
        dst[count]='\0';
    }
    void clear_error() noexcept {error_[0]='\0';}
    void append_error(const char* text) noexcept {
        const auto used=std::strlen(error_);
        if (used<sizeof(error_)) copy(error_+used,sizeof(error_)-used,text);
    }
    void set_error(const char* message,const char* detail=nullptr) noexcept {
        copy(error_,sizeof(error_),message);
        if (detail && *detail) {append_error(": ");append_error(detail);}
    }
    bool fail(const char* message) noexcept {set_error(message);return false;}
    void set_api_error(const char* context,int code) noexcept {
        const char* detail=SoapySDRDevice_lastError();
        if (!detail || !*detail) detail=SoapySDR_errToStr(code);
        set_error(context,detail);
    }
    bool getter_ok(const char* context) noexcept {
        const int status=SoapySDRDevice_lastStatus();
        if (status==0) return true;
        set_api_error(context,status);return false;
    }
    void preserve_error_and_unmake() noexcept {
        char saved[ErrorCapacity];copy(saved,sizeof(saved),error_);
        if (device_) {(void)SoapySDRDevice_unmake(device_);device_=nullptr;}
        configured_=false;cfg_={};copy(error_,sizeof(error_),saved);
    }

    char device_args_[ArgsCapacity]{};
    char error_[ErrorCapacity]{};
    SoapySDRDevice* device_{nullptr};
    SoapySDRStream* stream_{nullptr};
    ReceiverConfig cfg_{};
    bool args_valid_{true};
    bool configured_{false};
    bool running_{false};
};

}
#endif

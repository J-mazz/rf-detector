#include <cstdio>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#ifndef RF_IMPORT_STD
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#endif
#include <SoapySDR/Device.h>
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.acquire;
import rf.soapy;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c); std::exit(1); } } while (0)

struct SoapySDRDevice {};
struct SoapySDRStream {};

namespace fake {
SoapySDRDevice device;
SoapySDRStream stream;
int status=0;
const char* error="";
const char* native_format="CS16";
double full_scale=2048.0;
double frequency=0,rate=0,bandwidth=0,gain=0;
int make_calls=0,unmake_calls=0,setup_calls=0,activate_calls=0;
int deactivate_calls=0,close_calls=0,free_calls=0,read_calls=0;
int set_failure=0,activate_result=0,read_result=3,read_flags=0;
long long read_time=123456789;
long read_timeout=0;
std::size_t read_capacity=0;
void* read_buffer=nullptr;

void reset() {
    status=0;error="";native_format="CS16";full_scale=2048.0;
    frequency=rate=bandwidth=gain=0;
    make_calls=unmake_calls=setup_calls=activate_calls=0;
    deactivate_calls=close_calls=free_calls=read_calls=0;
    set_failure=activate_result=0;read_result=3;read_flags=SOAPY_SDR_HAS_TIME;
    read_time=123456789;read_timeout=0;read_capacity=0;read_buffer=nullptr;
}
}

extern "C" {
SoapySDRDevice* SoapySDRDevice_makeStrArgs(const char*) { ++fake::make_calls; return &fake::device; }
int SoapySDRDevice_unmake(SoapySDRDevice*) { ++fake::unmake_calls; return 0; }
int SoapySDRDevice_lastStatus(void) { return fake::status; }
const char* SoapySDRDevice_lastError(void) { return fake::error; }
const char* SoapySDR_errToStr(const int code) { return code==SOAPY_SDR_TIMEOUT?"TIMEOUT":code==SOAPY_SDR_OVERFLOW?"OVERFLOW":"STREAM_ERROR"; }
void SoapySDR_free(void* p) { ++fake::free_calls; std::free(p); }
std::size_t SoapySDRDevice_getNumChannels(const SoapySDRDevice*,const int) { fake::status=0; return 1; }
int SoapySDRDevice_setFrequency(SoapySDRDevice*,int,std::size_t,double v,const SoapySDRKwargs*) { if(fake::set_failure==1)return -2;fake::frequency=v;return 0; }
double SoapySDRDevice_getFrequency(const SoapySDRDevice*,int,std::size_t) { fake::status=0;return fake::frequency+125.0; }
int SoapySDRDevice_setSampleRate(SoapySDRDevice*,int,std::size_t,double v) { if(fake::set_failure==2)return -2;fake::rate=v;return 0; }
double SoapySDRDevice_getSampleRate(const SoapySDRDevice*,int,std::size_t) { fake::status=0;return fake::rate; }
int SoapySDRDevice_setBandwidth(SoapySDRDevice*,int,std::size_t,double v) { if(fake::set_failure==3)return -2;fake::bandwidth=v;return 0; }
double SoapySDRDevice_getBandwidth(const SoapySDRDevice*,int,std::size_t) { fake::status=0;return fake::bandwidth; }
int SoapySDRDevice_setGain(SoapySDRDevice*,int,std::size_t,double v) { if(fake::set_failure==4)return -2;fake::gain=v;return 0; }
double SoapySDRDevice_getGain(const SoapySDRDevice*,int,std::size_t) { fake::status=0;return fake::gain-0.5; }
char* SoapySDRDevice_getNativeStreamFormat(const SoapySDRDevice*,int,std::size_t,double* scale) {
    fake::status=0;*scale=fake::full_scale;
    const auto n=std::strlen(fake::native_format)+1;auto* p=static_cast<char*>(std::malloc(n));std::memcpy(p,fake::native_format,n);return p;
}
SoapySDRStream* SoapySDRDevice_setupStream(SoapySDRDevice*,int,const char* format,const std::size_t* channels,std::size_t count,const SoapySDRKwargs*) {
    ++fake::setup_calls;CHECK(std::strcmp(format,SOAPY_SDR_CS16)==0);CHECK(count==1 && channels && channels[0]==0);return &fake::stream;
}
int SoapySDRDevice_activateStream(SoapySDRDevice*,SoapySDRStream*,int,long long,std::size_t) { ++fake::activate_calls;return fake::activate_result; }
int SoapySDRDevice_deactivateStream(SoapySDRDevice*,SoapySDRStream*,int,long long) { ++fake::deactivate_calls;return 0; }
int SoapySDRDevice_closeStream(SoapySDRDevice*,SoapySDRStream*) { ++fake::close_calls;return 0; }
int SoapySDRDevice_readStream(SoapySDRDevice*,SoapySDRStream*,void* const* buffs,std::size_t count,int* flags,long long* time,long timeout) {
    ++fake::read_calls;fake::read_buffer=buffs[0];fake::read_capacity=count;fake::read_timeout=timeout;*flags=fake::read_flags;*time=fake::read_time;return fake::read_result;
}
}

static rf::acquire::ReceiverConfig requested() { return {915e6,2.4e6,2.0e6,31.0f,42,1.0f/32768.0f,9}; }

static void configure_start_read_stop() {
    fake::reset();
    {
        rf::acquire::SoapySource source("driver=fake");
        CHECK(fake::make_calls==0);
        CHECK(source.configure(requested()));
        CHECK(fake::make_calls==1 && fake::setup_calls==0);
        CHECK(source.config().center_frequency_hz==915e6+125.0);
        CHECK(source.config().sample_rate_sps==2.4e6);
        CHECK(source.config().bandwidth_hz==2.0e6);
        CHECK(source.config().gain_db==30.5f);
        CHECK(source.config().sample_scale==1.0f/2048.0f);
        CHECK(source.config().receiver_id==42 && source.config().config_id==9);
        CHECK(fake::free_calls==1);
        CHECK(source.start());CHECK(fake::setup_calls==1 && fake::activate_calls==1);
        std::int16_t iq[16]{};auto r=source.read_into(iq,8,7654321);
        CHECK(r.status==rf::acquire::ReadStatus::ok && r.complex_samples==3);
        CHECK(r.hw_time_valid && r.hw_time_ns==fake::read_time);
        CHECK(fake::read_buffer==iq && fake::read_capacity==8 && fake::read_timeout==7654321);
        fake::read_result=SOAPY_SDR_TIMEOUT;r=source.read_into(iq,8,9);CHECK(r.status==rf::acquire::ReadStatus::timeout && !r.hw_time_valid);
        fake::read_result=SOAPY_SDR_OVERFLOW;r=source.read_into(iq,8,9);CHECK(r.status==rf::acquire::ReadStatus::overflow && !r.hw_time_valid);
        fake::read_result=2;fake::read_flags=SOAPY_SDR_END_ABRUPT|SOAPY_SDR_HAS_TIME;
        r=source.read_into(iq,8,9);CHECK(r.status==rf::acquire::ReadStatus::overflow && r.complex_samples==2 && r.hw_time_valid);
        source.stop();source.stop();CHECK(fake::deactivate_calls==1 && fake::close_calls==1);
        CHECK(source.read_into(iq,8,9).status==rf::acquire::ReadStatus::stopped);
    }
    CHECK(fake::unmake_calls==1);
}

static void failure_paths_preserve_diagnostics() {
    fake::reset();fake::native_format="CF32";
    {
        rf::acquire::SoapySource source("");
        CHECK(!source.configure(requested()));
        CHECK(std::strstr(source.last_error(),"CF32")!=nullptr);
        CHECK(std::strstr(source.last_error(),"scale")!=nullptr);
        CHECK(fake::unmake_calls==1 && fake::free_calls==1);
    }
    fake::reset();fake::set_failure=3;fake::error="driver rejected bandwidth";
    {
        rf::acquire::SoapySource source("driver=fake");CHECK(!source.configure(requested()));
        CHECK(std::strstr(source.last_error(),"bandwidth")!=nullptr);
        CHECK(std::strstr(source.last_error(),"driver rejected bandwidth")!=nullptr);
    }
    fake::reset();
    {
        rf::acquire::SoapySource source("driver=fake");CHECK(source.configure(requested()));
        fake::activate_result=SOAPY_SDR_STREAM_ERROR;fake::error="activate failed";
        CHECK(!source.start());CHECK(fake::close_calls==1);
        CHECK(std::strstr(source.last_error(),"activate failed")!=nullptr);
        fake::activate_result=0;CHECK(source.start());source.stop();
    }
}

int main() {
    configure_start_read_stop();
    failure_paths_preserve_diagnostics();
    std::puts("test_soapy: PASS (C API shim; no hardware exercised)");
}

#include <cstdio>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
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
std::uintptr_t read_buffer=0;
bool make_failure=false,setup_failure=false,format_null=false;
int channels=1,channel_status=0,getter_failure=0,format_status=0;
int deactivate_result=0,close_result=0;
bool invalid_readback=false;
void getter_status(int field) { status=getter_failure==field?-2:0; }

void reset() {
    status=0;error="";native_format="CS16";full_scale=2048.0;
    frequency=rate=bandwidth=gain=0;
    make_calls=unmake_calls=setup_calls=activate_calls=0;
    deactivate_calls=close_calls=free_calls=read_calls=0;
    set_failure=activate_result=0;read_result=3;read_flags=SOAPY_SDR_HAS_TIME;
    read_time=123456789;read_timeout=0;read_capacity=0;read_buffer=0;
    make_failure=setup_failure=format_null=invalid_readback=false;
    channels=1;channel_status=getter_failure=format_status=deactivate_result=close_result=0;
}
}

extern "C" {
SoapySDRDevice* SoapySDRDevice_makeStrArgs(const char*) { ++fake::make_calls; return fake::make_failure?nullptr:&fake::device; }
int SoapySDRDevice_unmake(SoapySDRDevice*) { ++fake::unmake_calls; return 0; }
int SoapySDRDevice_lastStatus(void) { return fake::status; }
const char* SoapySDRDevice_lastError(void) { return fake::error; }
const char* SoapySDR_errToStr(const int code) { return code==SOAPY_SDR_TIMEOUT?"TIMEOUT":code==SOAPY_SDR_OVERFLOW?"OVERFLOW":"STREAM_ERROR"; }
void SoapySDR_free(void* p) { ++fake::free_calls; std::free(p); }
std::size_t SoapySDRDevice_getNumChannels(const SoapySDRDevice*,const int) { fake::status=fake::channel_status; return fake::channels; }
int SoapySDRDevice_setFrequency(SoapySDRDevice*,int,std::size_t,double v,const SoapySDRKwargs*) { if(fake::set_failure==1)return -2;fake::frequency=v;return 0; }
double SoapySDRDevice_getFrequency(const SoapySDRDevice*,int,std::size_t) { fake::getter_status(1);return fake::frequency+125.0; }
int SoapySDRDevice_setSampleRate(SoapySDRDevice*,int,std::size_t,double v) { if(fake::set_failure==2)return -2;fake::rate=v;return 0; }
double SoapySDRDevice_getSampleRate(const SoapySDRDevice*,int,std::size_t) { fake::getter_status(2);return fake::invalid_readback?0:fake::rate; }
int SoapySDRDevice_setBandwidth(SoapySDRDevice*,int,std::size_t,double v) { if(fake::set_failure==3)return -2;fake::bandwidth=v;return 0; }
double SoapySDRDevice_getBandwidth(const SoapySDRDevice*,int,std::size_t) { fake::getter_status(3);return fake::bandwidth; }
int SoapySDRDevice_setGain(SoapySDRDevice*,int,std::size_t,double v) { if(fake::set_failure==4)return -2;fake::gain=v;return 0; }
double SoapySDRDevice_getGain(const SoapySDRDevice*,int,std::size_t) { fake::getter_status(4);return fake::gain-0.5; }
char* SoapySDRDevice_getNativeStreamFormat(const SoapySDRDevice*,int,std::size_t,double* scale) {
    fake::status=fake::format_status;*scale=fake::full_scale;
    if(fake::format_null)return nullptr;
    const auto n=std::strlen(fake::native_format)+1;auto* p=static_cast<char*>(std::malloc(n));std::memcpy(p,fake::native_format,n);return p;
}
SoapySDRStream* SoapySDRDevice_setupStream(SoapySDRDevice*,int,const char* format,const std::size_t* channels,std::size_t count,const SoapySDRKwargs*) {
    ++fake::setup_calls;CHECK(std::strcmp(format,SOAPY_SDR_CS16)==0);CHECK(count==1 && channels && channels[0]==0);return fake::setup_failure?nullptr:&fake::stream;
}
int SoapySDRDevice_activateStream(SoapySDRDevice*,SoapySDRStream*,int,long long,std::size_t) { ++fake::activate_calls;return fake::activate_result; }
int SoapySDRDevice_deactivateStream(SoapySDRDevice*,SoapySDRStream*,int,long long) { ++fake::deactivate_calls;return fake::deactivate_result; }
int SoapySDRDevice_closeStream(SoapySDRDevice*,SoapySDRStream*) { ++fake::close_calls;return fake::close_result; }
int SoapySDRDevice_readStream(SoapySDRDevice*,SoapySDRStream*,void* const* buffs,std::size_t count,int* flags,long long* time,long timeout) {
    ++fake::read_calls;fake::read_buffer=reinterpret_cast<std::uintptr_t>(buffs[0]);fake::read_capacity=count;fake::read_timeout=timeout;*flags=fake::read_flags;*time=fake::read_time;return fake::read_result;
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
        CHECK(fake::read_buffer==reinterpret_cast<std::uintptr_t>(iq) && fake::read_capacity==8 && fake::read_timeout==7654321);
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

static void configuration_failures_and_retry() {
    for(int stage=1;stage<=4;++stage){
        fake::reset();fake::set_failure=stage;fake::error="setter diagnostic";
        rf::acquire::SoapySource source(nullptr);CHECK(!source.configure(requested()));
        CHECK(fake::unmake_calls==1 && fake::setup_calls==0);
        CHECK(std::strstr(source.last_error(),"setter diagnostic"));
        CHECK(source.config().sample_rate_sps==0);
        fake::set_failure=0;CHECK(source.configure(requested()));
        CHECK(source.last_error()[0]=='\0');CHECK(!source.configure(requested()));
        CHECK(fake::make_calls==2);
    }
    for(int stage=1;stage<=4;++stage){
        fake::reset();fake::getter_failure=stage;fake::error="readback diagnostic";
        rf::acquire::SoapySource source("");CHECK(!source.configure(requested()));
        CHECK(fake::unmake_calls==1 && std::strstr(source.last_error(),"readback diagnostic"));
        fake::getter_failure=0;CHECK(source.configure(requested()));
    }
    for(int mode=0;mode<7;++mode){
        fake::reset();
        if(mode==0)fake::make_failure=true;
        if(mode==1)fake::channels=0;
        if(mode==2)fake::channel_status=-2;
        if(mode==3)fake::format_null=true;
        if(mode==4)fake::format_status=-2;
        if(mode==5){fake::format_status=-2;fake::format_null=true;}
        if(mode==6)fake::invalid_readback=true;
        rf::acquire::SoapySource source("");CHECK(!source.configure(requested()));
        CHECK(source.last_error()[0]!='\0' && !source.start());
        CHECK(fake::unmake_calls==(mode==0?0:1));
        if(mode==4)CHECK(fake::free_calls==1);
        if(mode==3 || mode==5)CHECK(fake::free_calls==0);
    }
    for(double scale:{0.0,0.5,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),1e300}){
        fake::reset();fake::full_scale=scale;
        rf::acquire::SoapySource source("");CHECK(!source.configure(requested()));
        CHECK(fake::free_calls==1 && fake::unmake_calls==1);
    }
    fake::reset();
    {
        const std::string long_args(1024,'x');rf::acquire::SoapySource source(long_args.c_str());
        CHECK(!source.configure(requested()));CHECK(std::strstr(source.last_error(),"arguments exceed"));
        CHECK(fake::make_calls==0);
    }
    {
        const std::string max_args(1023,'x');rf::acquire::SoapySource source(max_args.c_str());
        auto invalid=requested();invalid.sample_scale=0;
        CHECK(!source.configure(invalid) && fake::make_calls==0);
        CHECK(source.configure(requested()));
    }
}

static void stream_boundaries_and_cleanup() {
    using Status=rf::acquire::ReadStatus;
    fake::reset();
    {
        rf::acquire::SoapySource source("");CHECK(!source.start());CHECK(source.configure(requested()));
        fake::setup_failure=true;CHECK(!source.start());CHECK(fake::activate_calls==0 && fake::close_calls==0);
        fake::setup_failure=false;CHECK(source.start());CHECK(!source.start() && !source.configure(requested()));
        std::int16_t iq[16]{};
        CHECK(source.read_into(nullptr,8,0).status==Status::failure);
        CHECK(source.read_into(iq,0,0).status==Status::failure);CHECK(fake::read_calls==0);
        fake::read_result=9;auto r=source.read_into(iq,8,0);
        CHECK(r.status==Status::failure && r.complex_samples==0 && !r.hw_time_valid);
        CHECK(std::strstr(source.last_error(),"more samples"));
        fake::read_result=0;r=source.read_into(iq,8,0);
        CHECK(r.status==Status::timeout && r.complex_samples==0 && !r.hw_time_valid);
        fake::read_result=SOAPY_SDR_STREAM_ERROR;fake::error="";
        r=source.read_into(iq,8,0);CHECK(r.status==Status::failure && r.complex_samples==0 && !r.hw_time_valid);
        CHECK(std::strstr(source.last_error(),"STREAM_ERROR"));
        fake::read_result=8;fake::read_flags=0;
        r=source.read_into(iq,8,std::numeric_limits<std::uint32_t>::max());
        CHECK(r.status==Status::ok && r.complex_samples==8 && !r.hw_time_valid);
        const auto expected=std::min<std::uint64_t>(std::numeric_limits<std::uint32_t>::max(),std::numeric_limits<long>::max());
        CHECK(static_cast<std::uint64_t>(fake::read_timeout)==expected);
        fake::read_flags=SOAPY_SDR_HAS_TIME;fake::read_time=-1;
        r=source.read_into(iq,8,0);CHECK(r.hw_time_valid && r.hw_time_ns==-1);
        // Destructor must stop and close an active stream exactly once.
    }
    CHECK(fake::deactivate_calls==1 && fake::close_calls==1 && fake::unmake_calls==1);
    for(int mode=0;mode<3;++mode){
        fake::reset();rf::acquire::SoapySource source("");CHECK(source.configure(requested()) && source.start());
        fake::deactivate_result=mode==0?0:-2;fake::close_result=mode==1?0:-2;
        source.stop();CHECK(fake::deactivate_calls==1 && fake::close_calls==1);
        CHECK(std::strstr(source.last_error(),mode==0?"close failed":"deactivation failed"));
        source.stop();CHECK(fake::deactivate_calls==1 && fake::close_calls==1);
        fake::deactivate_result=fake::close_result=0;CHECK(source.start());CHECK(source.last_error()[0]=='\0');
    }
    fake::reset();
    {
        rf::acquire::SoapySource source("");CHECK(source.configure(requested()));
        const std::string error(1000,'x');fake::error=error.c_str();fake::activate_result=-2;
        CHECK(!source.start());CHECK(std::strlen(source.last_error())==511);
        CHECK(std::strstr(source.last_error(),"activation failed"));fake::error="";
    }
}

int main() {
    configure_start_read_stop();
    failure_paths_preserve_diagnostics();
    configuration_failures_and_retry();
    stream_boundaries_and_cleanup();
    std::puts("test_soapy: PASS (C API shim; no hardware exercised)");
}

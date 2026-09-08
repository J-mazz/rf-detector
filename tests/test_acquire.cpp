#include <cstdio> // stderr and CHECK diagnostics also require macros with import std.
#include <new>
#ifndef RF_IMPORT_STD
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <vector>
#endif
#include <fcntl.h>
#include <unistd.h>
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.acquire;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c);std::exit(1);}}while(0)
static rf::acquire::ReceiverConfig config(){return {800e6,30.72e6,30e6,40,7};}
static std::vector<std::int16_t> capture(std::size_t part){
    rf::acquire::MockSource s;CHECK(s.configure(config()));CHECK(s.start());
    std::vector<std::int16_t> data(700000);std::size_t pos=0;
    while(pos<data.size()/2){auto r=s.read_into(data.data()+2*pos,std::min(part,data.size()/2-pos),1);CHECK(r.status==rf::acquire::ReadStatus::ok);pos+=r.complex_samples;}
    s.stop();return data;
}
static void configuration_and_lifecycle(){
    using C=rf::acquire::ReceiverConfig;
    using Change=void(*)(C&);
    const Change invalid[]={
        +[](C& c){c.center_frequency_hz=0;}, +[](C& c){c.center_frequency_hz=-1;},
        +[](C& c){c.center_frequency_hz=std::numeric_limits<double>::infinity();},
        +[](C& c){c.sample_rate_sps=0;}, +[](C& c){c.sample_rate_sps=-1;},
        +[](C& c){c.sample_rate_sps=1e9+1;}, +[](C& c){c.sample_rate_sps=std::numeric_limits<double>::quiet_NaN();},
        +[](C& c){c.bandwidth_hz=0;}, +[](C& c){c.bandwidth_hz=-1;},
        +[](C& c){c.bandwidth_hz=c.sample_rate_sps+1;}, +[](C& c){c.bandwidth_hz=std::numeric_limits<double>::infinity();},
        +[](C& c){c.gain_db=std::numeric_limits<float>::quiet_NaN();},
        +[](C& c){c.sample_scale=0;}, +[](C& c){c.sample_scale=-1;},
        +[](C& c){c.sample_scale=1.01f;}, +[](C& c){c.sample_scale=std::numeric_limits<float>::infinity();}
    };
    for(auto change:invalid){auto c=config();change(c);CHECK(!rf::acquire::valid_config(c));}
    auto boundary=config();boundary.sample_rate_sps=boundary.bandwidth_hz=1e9;boundary.sample_scale=1;boundary.gain_db=-10;
    CHECK(rf::acquire::valid_config(boundary));
    rf::acquire::MockSource source(false,0),same_seed(false,1);
    std::int16_t iq[20],expected[20];std::fill_n(iq,20,std::int16_t{12345});
    CHECK(!source.start() && !source.lossless());
    CHECK(source.read_into(iq,10,0).status==rf::acquire::ReadStatus::stopped);
    CHECK(source.configure(config()) && source.start());
    CHECK(!source.start() && !source.configure(boundary));
    CHECK(source.config().sample_rate_sps==config().sample_rate_sps);
    CHECK(source.read_into(nullptr,10,0).status==rf::acquire::ReadStatus::failure);
    CHECK(source.read_into(iq,0,0).status==rf::acquire::ReadStatus::failure);
    CHECK(iq[0]==12345);
    CHECK(same_seed.configure(config()) && same_seed.start());
    for(int read=1;read<=5;++read){
        const auto r=source.read_into(iq,10,0),reference=same_seed.read_into(expected,10,0);
        CHECK(r.status==rf::acquire::ReadStatus::ok && r.complex_samples==(read==5?6u:10u));
        CHECK(r.complex_samples==reference.complex_samples && r.hw_time_ns==reference.hw_time_ns);
        CHECK(std::equal(iq,iq+2*r.complex_samples,expected));
    }
    source.stop();source.stop();CHECK(source.start());
    const auto restart=source.read_into(iq,10,0);CHECK(restart.hw_time_valid && restart.hw_time_ns==0);
    same_seed.stop();CHECK(same_seed.start());CHECK(same_seed.read_into(expected,10,0).complex_samples==10);
    CHECK(std::equal(iq,iq+20,expected));source.stop();same_seed.stop();
}

static void replay_boundaries(){
    using Status=rf::acquire::ReadStatus;
    char path[]="/tmp/rf-replay-boundary-XXXXXX";const int fd=mkstemp(path);CHECK(fd>=0);
    std::int16_t iq[12];std::fill_n(iq,12,std::int16_t{12345});
    rf::acquire::ReplaySource source(path,2);
    CHECK(!source.start());CHECK(source.read_into(iq,2,0).status==Status::stopped);
    CHECK(source.configure(config()) && source.start());
    CHECK(!source.start() && !source.configure(config()));
    CHECK(source.read_into(iq,2,0).status==Status::stopped); // empty regular file
    source.stop();source.stop();
    const unsigned char data[]={1,0,2,0,3,0,4,0,5,0,6,0};CHECK(write(fd,data,sizeof(data))==sizeof(data));
    CHECK(source.start());
    CHECK(source.read_into(nullptr,2,0).status==Status::failure);
    CHECK(source.read_into(iq,0,0).status==Status::failure);
    CHECK(source.read_into(iq,6,0).complex_samples==2);
    CHECK(iq[0]==1 && iq[1]==2 && iq[2]==3 && iq[3]==4 && iq[4]==12345);
    auto r=source.read_into(iq,6,0);CHECK(r.status==Status::ok && r.complex_samples==1 && !r.hw_time_valid);
    CHECK(iq[0]==5 && iq[1]==6 && iq[2]==3);
    CHECK(source.read_into(iq,6,0).status==Status::stopped);source.stop();
    CHECK(source.start());CHECK(source.read_into(iq,1,0).complex_samples==1 && iq[0]==1 && iq[1]==2);
    source.stop();
    rf::acquire::ReplaySource zero(path,0);CHECK(!zero.configure(config()));
    rf::acquire::ReplaySource huge(path,std::numeric_limits<std::size_t>::max());
    CHECK(huge.configure(config()) && huge.start());
    CHECK(huge.read_into(iq,std::numeric_limits<std::size_t>::max(),0).status==Status::failure);huge.stop();
    // A file may become malformed after start-time validation.
    CHECK(source.start());CHECK(ftruncate(fd,3)==0);
    r=source.read_into(iq,2,0);CHECK(r.status==Status::failure && r.complex_samples==0 && !r.hw_time_valid);
    source.stop();CHECK(close(fd)==0);CHECK(unlink(path)==0);CHECK(!source.start());
    rf::acquire::ReplaySource directory("/tmp");CHECK(directory.configure(config()) && !directory.start());
    rf::acquire::ReplaySource device("/dev/null");CHECK(device.configure(config()) && !device.start());
}
int main(){
    configuration_and_lifecycle();replay_boundaries();
    CHECK(capture(131072)==capture(127));

    // A paced read may wait only through its caller-supplied deadline. A
    // timeout must not consume a sample or advance its hardware timestamp.
    auto slow_config=config();slow_config.sample_rate_sps=5;slow_config.bandwidth_hz=5;
    rf::acquire::MockSource paced(true);CHECK(paced.configure(slow_config));CHECK(paced.start());
    std::int16_t paced_iq[2];
    const auto timeout_started=std::chrono::steady_clock::now();
    auto paced_result=paced.read_into(paced_iq,1,1000);
    const auto timeout_elapsed=std::chrono::steady_clock::now()-timeout_started;
    CHECK(paced_result.status==rf::acquire::ReadStatus::timeout && paced_result.complex_samples==0 && !paced_result.hw_time_valid);
    CHECK(timeout_elapsed<std::chrono::milliseconds(100));
    rf::core::sleep_ns(210'000'000);
    paced_result=paced.read_into(paced_iq,1,1000);
    CHECK(paced_result.status==rf::acquire::ReadStatus::ok && paced_result.complex_samples==1 && paced_result.hw_time_valid && paced_result.hw_time_ns==0);
    paced_result=paced.read_into(paced_iq,1,1000);
    CHECK(paced_result.status==rf::acquire::ReadStatus::timeout && paced_result.complex_samples==0 && !paced_result.hw_time_valid);
    rf::core::sleep_ns(200'000'000);
    paced_result=paced.read_into(paced_iq,1,1000);
    CHECK(paced_result.status==rf::acquire::ReadStatus::ok && paced_result.complex_samples==1 && paced_result.hw_time_valid && paced_result.hw_time_ns==200'000'000);
    paced.stop();

    auto tiny_rate_config=config();tiny_rate_config.sample_rate_sps=1e-300;tiny_rate_config.bandwidth_hz=1e-300;
    rf::acquire::MockSource tiny_rate;CHECK(tiny_rate.configure(tiny_rate_config));CHECK(tiny_rate.start());
    auto tiny_rate_result=tiny_rate.read_into(paced_iq,1,1);
    CHECK(tiny_rate_result.status==rf::acquire::ReadStatus::ok && tiny_rate_result.hw_time_valid && tiny_rate_result.hw_time_ns==0);
    tiny_rate_result=tiny_rate.read_into(paced_iq,1,1);
    CHECK(tiny_rate_result.status==rf::acquire::ReadStatus::ok && !tiny_rate_result.hw_time_valid);
    tiny_rate.stop();

    rf::acquire::MockSource m;auto invalid=config();invalid.sample_scale=0;CHECK(!m.configure(invalid));
    invalid=config();invalid.sample_rate_sps=std::numeric_limits<double>::quiet_NaN();CHECK(!m.configure(invalid));
    char path[]="/tmp/rf-replay-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);
    const unsigned char bytes[]={0,128,255,127,1,0,255,255,42,0,24,0};CHECK(write(fd,bytes,sizeof(bytes))==sizeof(bytes));close(fd);
    rf::acquire::ReplaySource s(path,1);CHECK(s.configure(config()));CHECK(s.start());std::int16_t iq[2];
    auto r=s.read_into(iq,1,1);CHECK(r.complex_samples==1 && iq[0]==-32768 && iq[1]==32767 && !r.hw_time_valid);
    r=s.read_into(iq,1,1);CHECK(r.complex_samples==1 && iq[0]==1 && iq[1]==-1);
    r=s.read_into(iq,1,1);CHECK(r.complex_samples==1 && iq[0]==42 && iq[1]==24);
    CHECK(s.read_into(iq,1,1).status==rf::acquire::ReadStatus::stopped);CHECK(s.lossless());s.stop();
    fd=open(path,O_WRONLY|O_APPEND);CHECK(fd>=0);CHECK(write(fd,"x",1)==1);close(fd);
    CHECK(!s.start());unlink(path);std::puts("test_acquire: PASS");
}

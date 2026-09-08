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
int main(){
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

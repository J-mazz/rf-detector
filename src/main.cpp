// rfdet: mock, lossless raw-SC16 replay, or optional SoapySDR receiver.
#include <cerrno>
#include <cstdio>
#ifndef RF_IMPORT_STD
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.core;
import rf.memory;
import rf.runtime;
import rf.acquire;
import rf.pipeline;
import rf.signal;
import rf.health;
import rf.output;
#ifdef RF_WITH_SOAPY
import rf.soapy;
#endif
namespace {
volatile sig_atomic_t signalled=0;
void on_signal(int){signalled=1;}
void usage(){
    std::puts("rfdet [--input capture.sc16 | --device 'driver=bladerf'] [options]\n"
        "  --seconds N          run limit; default 3s mock, until EOF for replay\n"
        "  --no-lock            allow unlocked memory for development\n"
        "  --unpaced            generate mock input without real-time pacing\n"
        "  --events PATH        write begin/update/end event records to a new CSV\n"
        "  --status-ms N        health report interval, 10..60000 (default 250)\n"
        "  --stall-ms N         no-progress threshold, 10..60000 (default 1000)\n"
        "  --frequency HZ       center frequency (default 800000000)\n"
        "  --sample-rate SPS    sample rate (default 30720000)\n"
        "  --bandwidth HZ       usable capture bandwidth (default 30000000)\n"
        "  --gain DB            receiver gain (default 40)\n"
        "  --full-scale VALUE   replay/mock integer full scale (default 32768)\n"
        "  --read-size N        maximum replay read size, 1..131072\n"
        "  --fft N              power of two, 256..65536; hop is N/2\n"
        "  --threshold DB       start threshold above local background\n"
        "  --help\n"
        "Raw files contain little-endian interleaved signed int16 I,Q pairs.\n"
        "Frequency intervals are RF observations; no vehicle classifier is included.");
}
bool number(const char* text,double& out){char* end=nullptr;errno=0;out=std::strtod(text,&end);return end!=text && *end=='\0' && errno==0 && std::isfinite(out);}
void* acquisition(void* p){static_cast<rf::pipeline::PipelineSystem*>(p)->run_acquisition();return nullptr;}
void* dsp(void* p){static_cast<rf::pipeline::PipelineSystem*>(p)->run_dsp();return nullptr;}
void* sink(void* p){static_cast<rf::pipeline::PipelineSystem*>(p)->run_sink();return nullptr;}
}
int main(int argc,char** argv){
    const char* input=nullptr;const char* device=nullptr;const char* events_path=nullptr;
    bool locked=true,realtime=true,seconds_set=false;double seconds=3;std::size_t read_size=131072;
    std::uint64_t status_ms=250,stall_ms=1000;
    rf::acquire::ReceiverConfig rc{800e6,30.72e6,30e6,40,0};rf::pipeline::PipelineConfig pc;
    for(int i=1;i<argc;++i){
        const char* arg=argv[i];
        if(std::strcmp(arg,"--help")==0){usage();return 0;}
        if(std::strcmp(arg,"--no-lock")==0){locked=false;continue;}
        if(std::strcmp(arg,"--unpaced")==0){realtime=false;continue;}
        if(i+1>=argc){std::fprintf(stderr,"missing value for %s\n",arg);return 2;}
        const char* value=argv[++i];
        if(std::strcmp(arg,"--input")==0){input=value;continue;}
        if(std::strcmp(arg,"--device")==0){device=value;continue;}
        if(std::strcmp(arg,"--events")==0){events_path=value;continue;}
        double v;if(!number(value,v)){std::fprintf(stderr,"invalid numeric value for %s\n",arg);return 2;}
        if(std::strcmp(arg,"--seconds")==0){if(v<0)return 2;seconds=v;seconds_set=true;}
        else if(std::strcmp(arg,"--status-ms")==0 || std::strcmp(arg,"--stall-ms")==0){
            if(v<10 || v>60000 || std::floor(v)!=v)return 2;
            if(std::strcmp(arg,"--status-ms")==0)status_ms=static_cast<std::uint64_t>(v);
            else stall_ms=static_cast<std::uint64_t>(v);
        }
        else if(std::strcmp(arg,"--frequency")==0)rc.center_frequency_hz=v;
        else if(std::strcmp(arg,"--sample-rate")==0)rc.sample_rate_sps=v;
        else if(std::strcmp(arg,"--bandwidth")==0)rc.bandwidth_hz=v;
        else if(std::strcmp(arg,"--gain")==0)rc.gain_db=static_cast<float>(v);
        else if(std::strcmp(arg,"--full-scale")==0){if(v<1 || v>32768)return 2;rc.sample_scale=static_cast<float>(1/v);}
        else if(std::strcmp(arg,"--threshold")==0)pc.detector.threshold_db=static_cast<float>(v);
        else if(std::strcmp(arg,"--read-size")==0){if(v<1 || v>131072 || std::floor(v)!=v)return 2;read_size=static_cast<std::size_t>(v);}
        else if(std::strcmp(arg,"--fft")==0){if(v<256 || v>65536 || std::floor(v)!=v)return 2;pc.detector.fft_size=static_cast<std::size_t>(v);pc.detector.hop=pc.detector.fft_size/2;}
        else{std::fprintf(stderr,"unknown option: %s\n",arg);return 2;}
    }
    if(input && device){std::fputs("choose either --input or --device\n",stderr);return 2;}
#ifndef RF_WITH_SOAPY
    if(device){std::fputs("this build has no SoapySDR adapter; build with --soapy\n",stderr);return 2;}
#endif
    if((input || device) && !seconds_set)seconds=0;
    rf::acquire::MockSource mock(realtime);rf::acquire::ReplaySource replay(input,read_size);
    rf::acquire::SDRSource* source=input?static_cast<rf::acquire::SDRSource*>(&replay):&mock;
#ifdef RF_WITH_SOAPY
    rf::acquire::SoapySource radio(device?device:"");if(device)source=&radio;
#endif
    if(!source->configure(rc)){
        std::fputs("receiver configuration failed\n",stderr);
#ifdef RF_WITH_SOAPY
        if(device)std::fprintf(stderr,"%s\n",radio.last_error());
#endif
        return 2;
    }
    rf::runtime::StageState state;rf::output::CsvWriter csv(state);
    if(events_path){
        if(!csv.open(events_path)){std::perror("events output (must be a new file)");return 2;}
        pc.sink=rf::output::CsvWriter::callback;pc.sink_context=&csv;
    }
    rf::pipeline::PipelineSystem pipeline(*source,state,pc);
    const auto e=pipeline.initialize(locked?rf::memory::DefaultRegionConfig:rf::memory::UnlockedRegionConfig);
    if(e!=rf::memory::MemoryError::none){
        std::fprintf(stderr,"initialization failed (memory/config error %u); memlock limit=%zu bytes\n",unsigned(e),rf::memory::memlock_limit_bytes());
        return 3;
    }
    if(!source->start()){
        std::fputs("receiver start failed\n",stderr);
#ifdef RF_WITH_SOAPY
        if(device)std::fprintf(stderr,"%s\n",radio.last_error());
#endif
        return 4;
    }
    const auto started=rf::core::monotonic_now_ns();
    rf::health::Monitor monitor(started,stall_ms*1000000);
    auto report_health=[&](std::uint64_t now){
        if(!rf::output::write_health(stderr,monitor.poll(state,now))){
            state.fatal_error.store(true);state.request_stop();
        }
    };
    report_health(started);
    struct sigaction action{};action.sa_handler=on_signal;sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,nullptr);sigaction(SIGTERM,&action,nullptr);
    sigset_t blocked,previous;sigemptyset(&blocked);sigaddset(&blocked,SIGINT);sigaddset(&blocked,SIGTERM);
    pthread_sigmask(SIG_BLOCK,&blocked,&previous);
    void* (*workers[3])(void*)={acquisition,dsp,sink};pthread_t threads[3];bool created[3]{};
    for(int k=0;k<3;++k){
        const int error=pthread_create(&threads[k],nullptr,workers[k],&pipeline);
        if(error){std::fprintf(stderr,"thread creation failed: %s\n",std::strerror(error));state.fatal_error.store(true);state.request_stop();workers[k](&pipeline);}
        else created[k]=true;
    }
    pthread_sigmask(SIG_SETMASK,&previous,nullptr);
    auto last_report=started;
    while(!signalled && !state.stopping()){
        const auto now=rf::core::monotonic_now_ns();
        if(seconds!=0 && double(now-started)*1e-9>=seconds)break;
        if(now-last_report>=status_ms*1000000){report_health(now);last_report=now;}
        rf::core::sleep_ns(10000000);
    }
    state.request_stop();for(int k=0;k<3;++k)if(created[k])pthread_join(threads[k],nullptr);
    source->stop();pipeline.reclaim_held_slots();
    (void)csv.close();
    report_health(rf::core::monotonic_now_ns());
    std::fprintf(stderr,"samples received=%llu admitted=%llu processed=%llu discarded=%llu unknown_gaps=%llu\n"
        "windows=%llu invalid_windows=%llu resets=%llu events=%llu consumed=%llu dropped_events=%llu\n"
        "free slots=%zu/%zu,%zu/%zu,%zu/%zu protocol_violations=%llu fatal=%d\n",
        (unsigned long long)state.samples_received.load(),(unsigned long long)state.samples_admitted.load(),
        (unsigned long long)state.samples_processed.load(),(unsigned long long)state.samples_discarded.load(),
        (unsigned long long)state.unknown_gap_events.load(),(unsigned long long)state.analysis_windows.load(),
        (unsigned long long)state.invalid_windows.load(),(unsigned long long)state.continuity_resets.load(),
        (unsigned long long)state.events_emitted.load(),(unsigned long long)state.events_consumed.load(),
        (unsigned long long)state.events_dropped.load(),pipeline.capture_slots_free(),rf::pipeline::Topology::CapturePoolDepth,
        pipeline.scratch_slots_free(),rf::pipeline::Topology::ScratchDepth,pipeline.event_slots_free(),rf::pipeline::Topology::EventPoolDepth,
        (unsigned long long)state.protocol_violations.load(),int(state.fatal_error.load()));
    return state.fatal_error.load() || state.protocol_violations.load()?5:0;
}

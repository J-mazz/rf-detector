#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <stdlib.h> // POSIX mkdtemp is not exported by import std.
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef RF_IMPORT_STD
#include <cstdlib>
#include <string>
#endif
#ifdef RF_IMPORT_STD
import std;
#endif
import rf.output;
import rf.runtime;
import rf.signal;
import rf.health;

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"failed: %s:%d %s\n",__FILE__,__LINE__,#c); std::exit(1); } } while (0)

static std::string contents(const char* path) {
    FILE* file=std::fopen(path,"r"); CHECK(file);
    std::string result; char buffer[1024];
    while (std::fgets(buffer,sizeof(buffer),file)) result+=buffer;
    CHECK(!std::ferror(file)); CHECK(std::fclose(file)==0); return result;
}

// Invalidate only this writer's descriptor, after its header has flushed.
// This exercises fclose failure without a production-only injection API.
static void invalidate_descriptor(const char* path) {
    struct stat expected{}; CHECK(::stat(path,&expected)==0);
    DIR* directory=::opendir("/proc/self/fd"); CHECK(directory);
    int found=-1;
    while (auto* entry=::readdir(directory)) {
        char* end=nullptr; const long fd=std::strtol(entry->d_name,&end,10);
        if (*end || end==entry->d_name) continue;
        struct stat actual{};
        if (::fstat(static_cast<int>(fd),&actual)==0 && actual.st_dev==expected.st_dev && actual.st_ino==expected.st_ino) {
            CHECK(found==-1); found=static_cast<int>(fd);
        }
    }
    CHECK(::closedir(directory)==0); CHECK(found>=0); CHECK(::close(found)==0);
}

int main() {
    char directory[]="/tmp/rf-output-XXXXXX"; CHECK(::mkdtemp(directory));
    const auto path=std::string(directory)+"/events.csv";
    rf::runtime::StageState state;
    const std::string header="event_id,segment_id,config_id,first_sample,end_sample,bin_begin,bin_end,frequency_hz,bandwidth_hz,power_db,background_db,excess_db,hw_time_valid,first_hw_time_ns,end_reason,phase\n";
    auto event=rf::signal::CandidateEvent{};
    event.event_id=18446744073709551615ull; event.source_meta.config_id=9;
    event.source_meta.center_frequency_hz=915000000;
    auto& d=event.detection; d.segment_id=7; d.first_sample=100; d.end_sample=356;
    d.bin_begin=150; d.bin_end=154; d.center_offset_hz=-125.5f; d.bandwidth_hz=250;
    d.energy_db=-20.25f; d.noise_floor_db=-40.5f; d.excess_over_background_db=20.25f;
    d.hw_time_valid=true; d.first_hw_time_ns=-123;
    d.end_reason=rf::signal::EventEnd::gap; d.phase=rf::signal::EventPhase::update;
    {
        rf::output::CsvWriter writer(state);
        CHECK(writer.close()); CHECK(writer.open(path.c_str()));
        CHECK(contents(path.c_str())==header); // visible before close
        errno=0; CHECK(!writer.open(path.c_str()) && errno==EBUSY);
        rf::output::CsvWriter::callback(event,&writer);
        CHECK(contents(path.c_str())==header+"18446744073709551615,7,9,100,356,150,154,914999874.500,250.000,-20.250,-40.500,20.250,1,-123,1,1\n");
        CHECK(writer.close() && writer.close());
        const auto saved=contents(path.c_str());
        CHECK(!writer.open(path.c_str()) && errno==EEXIST);
        CHECK(contents(path.c_str())==saved);
        CHECK(!writer.open((std::string(directory)+"/missing/file").c_str()));
        CHECK(!state.fatal_error && !state.stopping());
    }
    CHECK(::unlink(path.c_str())==0);
    {
        rf::runtime::StageState failed;rf::output::CsvWriter writer(failed);
        struct rlimit saved{};CHECK(::getrlimit(RLIMIT_FSIZE,&saved)==0);
        struct sigaction ignore{},previous{};ignore.sa_handler=SIG_IGN;sigemptyset(&ignore.sa_mask);
        CHECK(::sigaction(SIGXFSZ,&ignore,&previous)==0);
        auto limited=saved;limited.rlim_cur=0;CHECK(::setrlimit(RLIMIT_FSIZE,&limited)==0);
        const bool opened=writer.open(path.c_str());
        // Restore before assertions and process exit (including gcov writes).
        CHECK(::setrlimit(RLIMIT_FSIZE,&saved)==0);
        CHECK(::sigaction(SIGXFSZ,&previous,nullptr)==0);
        CHECK(!opened && failed.fatal_error && failed.stopping());
        (void)writer.close();
    }
    CHECK(::unlink(path.c_str())==0);
    {
        rf::runtime::StageState failed;
        rf::output::CsvWriter writer(failed); CHECK(writer.open(path.c_str()));
        invalidate_descriptor(path.c_str());
        CHECK(!writer.close()); CHECK(failed.fatal_error && failed.stopping());
        CHECK(writer.close());
    }
    CHECK(::unlink(path.c_str())==0);
    {
        rf::runtime::StageState failed;
        rf::output::CsvWriter writer(failed); CHECK(writer.open(path.c_str()));
        invalidate_descriptor(path.c_str());
        rf::output::CsvWriter::callback(event,&writer);
        CHECK(failed.fatal_error && failed.stopping());
        CHECK(!writer.close());
    }
    CHECK(::unlink(path.c_str())==0);
    {
        rf::runtime::StageState failed; rf::output::CsvWriter writer(failed);
        rf::output::CsvWriter::callback(event,&writer);
        CHECK(failed.fatal_error && failed.stopping());
    }
    // Destructor owns closing and flushing an open stream.
    { rf::output::CsvWriter writer(state); CHECK(writer.open(path.c_str())); }
    CHECK(contents(path.c_str())==header); CHECK(::unlink(path.c_str())==0);
    CHECK(::rmdir(directory)==0);

    FILE* health=std::tmpfile(); CHECK(health);
    rf::health::Snapshot snapshot{};
    snapshot.status=rf::health::Status::degraded; snapshot.timestamp_ns=123;
    snapshot.capture_age_ns=1500000; snapshot.dsp_age_ns=2500000;
    snapshot.received=11; snapshot.admitted=10; snapshot.processed=9; snapshot.discarded=1;
    snapshot.unknown_gaps=2; snapshot.windows=3; snapshot.invalid_windows=4;
    snapshot.resets=5; snapshot.dropped_events=6; snapshot.detector=rf::signal::DetectorHealth::uncertain;
    snapshot.capture_stalled=true; snapshot.sample_loss=true; snapshot.event_loss=true;
    CHECK(rf::output::write_health(health,snapshot)); std::rewind(health);
    char line[1024]; CHECK(std::fgets(line,sizeof(line),health));
    CHECK(std::string(line)=="health state=degraded timestamp_ns=123 capture_age_ms=1.500 dsp_age_ms=2.500 received=11 admitted=10 processed=9 discarded=1 unknown_gaps=2 windows=3 invalid_windows=4 resets=5 dropped_events=6 detector=2 capture_stalled=1 dsp_stalled=0 sample_loss=1 device_gap=0 event_loss=1 invalid_input=0\n");
    CHECK(std::fclose(health)==0);
    FILE* full=std::fopen("/dev/full","w"); CHECK(full);
    CHECK(!rf::output::write_health(full,snapshot)); (void)std::fclose(full);
    std::puts("test_output: PASS");
}

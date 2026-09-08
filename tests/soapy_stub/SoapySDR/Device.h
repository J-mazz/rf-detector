#pragma once
// Test-only subset of the SoapySDR 0.8 C API used by rf.soapy.
#include <stddef.h>

#define SOAPY_SDR_TX 0
#define SOAPY_SDR_RX 1
#define SOAPY_SDR_END_BURST (1 << 1)
#define SOAPY_SDR_HAS_TIME (1 << 2)
#define SOAPY_SDR_END_ABRUPT (1 << 3)
#define SOAPY_SDR_ONE_PACKET (1 << 4)
#define SOAPY_SDR_MORE_FRAGMENTS (1 << 5)
#define SOAPY_SDR_WAIT_TRIGGER (1 << 6)

#define SOAPY_SDR_TIMEOUT (-1)
#define SOAPY_SDR_STREAM_ERROR (-2)
#define SOAPY_SDR_CORRUPTION (-3)
#define SOAPY_SDR_OVERFLOW (-4)
#define SOAPY_SDR_NOT_SUPPORTED (-5)
#define SOAPY_SDR_TIME_ERROR (-6)
#define SOAPY_SDR_UNDERFLOW (-7)

#define SOAPY_SDR_CS16 "CS16"

typedef struct SoapySDRDevice SoapySDRDevice;
typedef struct SoapySDRStream SoapySDRStream;
typedef struct SoapySDRKwargs {
    size_t size;
    char** keys;
    char** vals;
} SoapySDRKwargs;

#ifdef __cplusplus
extern "C" {
#endif

SoapySDRDevice* SoapySDRDevice_makeStrArgs(const char* args);
int SoapySDRDevice_unmake(SoapySDRDevice* device);
int SoapySDRDevice_lastStatus(void);
const char* SoapySDRDevice_lastError(void);
const char* SoapySDR_errToStr(int errorCode);
void SoapySDR_free(void* ptr);

size_t SoapySDRDevice_getNumChannels(const SoapySDRDevice* device,int direction);
int SoapySDRDevice_setFrequency(SoapySDRDevice* device,int direction,size_t channel,double frequency,const SoapySDRKwargs* args);
double SoapySDRDevice_getFrequency(const SoapySDRDevice* device,int direction,size_t channel);
int SoapySDRDevice_setSampleRate(SoapySDRDevice* device,int direction,size_t channel,double rate);
double SoapySDRDevice_getSampleRate(const SoapySDRDevice* device,int direction,size_t channel);
int SoapySDRDevice_setBandwidth(SoapySDRDevice* device,int direction,size_t channel,double bandwidth);
double SoapySDRDevice_getBandwidth(const SoapySDRDevice* device,int direction,size_t channel);
int SoapySDRDevice_setGain(SoapySDRDevice* device,int direction,size_t channel,double gain);
double SoapySDRDevice_getGain(const SoapySDRDevice* device,int direction,size_t channel);
char* SoapySDRDevice_getNativeStreamFormat(const SoapySDRDevice* device,int direction,size_t channel,double* fullScale);

SoapySDRStream* SoapySDRDevice_setupStream(SoapySDRDevice* device,int direction,const char* format,const size_t* channels,size_t numChans,const SoapySDRKwargs* args);
int SoapySDRDevice_activateStream(SoapySDRDevice* device,SoapySDRStream* stream,int flags,long long timeNs,size_t numElems);
int SoapySDRDevice_deactivateStream(SoapySDRDevice* device,SoapySDRStream* stream,int flags,long long timeNs);
int SoapySDRDevice_closeStream(SoapySDRDevice* device,SoapySDRStream* stream);
int SoapySDRDevice_readStream(SoapySDRDevice* device,SoapySDRStream* stream,void* const* buffs,size_t numElems,int* flags,long long* timeNs,long timeoutUs);

#ifdef __cplusplus
}
#endif

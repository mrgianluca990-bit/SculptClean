#include "../Source/VoiceDenoiser.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> watch{false};
std::atomic<unsigned> cppAllocations{0}, cAllocations{0};
constexpr double pi = 3.14159265358979323846;
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
std::vector<float> fixture(int count, bool noiseOnly=false) {
    std::vector<float> result(static_cast<std::size_t>(count));
    std::uint32_t seed=12345;
    for(int n=0;n<count;++n){
        seed=seed*1664525U+1013904223U;
        const double noise=(static_cast<double>(seed)/4294967296.0-.5)*.04;
        const double envelope=(n%24000)<15000?1.0:.1;
        const double phase=2*pi*n/48000;
        result[static_cast<std::size_t>(n)]=static_cast<float>(noise+(noiseOnly?0:envelope*(.13*std::sin(173*phase)+.06*std::sin(346*phase)+.025*std::sin(692*phase))));
    }
    return result;
}
std::vector<float> render(sculptclean::VoiceDenoiser& voice, const std::vector<float>& input) {
    std::vector<float> out(input.size()+sculptclean::VoiceDenoiser::latencySamples);
    for(std::size_t n=0;n<out.size();++n){float l=n<input.size()?input[n]:0,r=l;voice.processSample(l,r,1);out[n]=l;}
    return out;
}
void dryAndRate() {
    sculptclean::VoiceDenoiser p;require(!p.prepare(44100,2),"unresampled 44.1 kHz incorrectly accepted");
    require(!p.prepare(std::numeric_limits<double>::quiet_NaN(),2),"invalid rate accepted");
    require(p.prepare(48000,2),"native48k preparation failed");
    const auto input=fixture(10000);
    std::array<std::array<float,960>,2> expected{};std::size_t cursor=0;
    for(float sample:input){
        float l=sample,r=-sample*.7f;const float a=expected[0][cursor],b=expected[1][cursor];
        expected[0][cursor]=l;expected[1][cursor]=r;cursor=(cursor+1)%960;
        p.processSample(l,r,2);require(l==a&&r==b,"zero amount is not exactly delayed dry");
    }
    std::cout<<"PASS: explicit 48k requirement and exact 960-sample dry latency\n";
}
void upstreamEquivalence() {
    const auto input=fixture(48000);
    sculptclean::VoiceDenoiser p;p.setAmount(1);require(p.prepare(48000,1),"prepare failed");
    const auto actual=render(p,input);
    auto* direct=rnnoise_create();require(direct!=nullptr,"direct state failed");
    std::array<float,480> frame{},out{};
    double maxError=0;
    for(std::size_t at=0;at<input.size()+480;at+=480){
        for(std::size_t n=0;n<480;++n)frame[n]=at+n<input.size()?input[at+n]*32768.0f:0;
        rnnoise_process_frame(direct,out.data(),frame.data());
        for(std::size_t n=0;n<480&&at+n+480<actual.size();++n)
            maxError=std::max(maxError,std::abs(static_cast<double>(actual[at+n+480])-out[n]/32768.0f));
    }
    rnnoise_destroy(direct);
    require(maxError<1e-7,"adapter output diverges from actual upstream RNNoise");
    p.reset();const auto again=render(p,input);require(actual==again,"RNNoise reset is not deterministic");
    std::cout<<"PASS: actual RNNoise frame output and deterministic reset; max error "<<maxError<<"\n";
}
void impulseTiming() {
    sculptclean::VoiceDenoiser p;p.setAmount(1);require(p.prepare(48000,1),"prepare failed");
    std::vector<float> input(10000);input[4000]=.9f;
    const auto result=render(p,input);
    const auto peak=static_cast<int>(std::distance(result.begin(),std::max_element(result.begin(),result.end(),[](float a,float b){return std::abs(a)<std::abs(b);})));
    require(peak==4960,"processed impulse main peak does not match960sample compensation");
    std::cout<<"PASS: processed impulse peak at input+960 samples (480 upstream +480 packet assembly)\n";
}
void reductionAndRealtime() {
    const auto noise=fixture(48000*4,true);
    sculptclean::VoiceDenoiser p;p.setAmount(1);require(p.prepare(48000,1),"prepare failed");
    const auto start=std::chrono::steady_clock::now();
    auto output=render(p,noise);
    const auto stop=std::chrono::steady_clock::now();
    double before=0,after=0;
    for(std::size_t n=48000;n<noise.size();++n){before+=static_cast<double>(noise[n])*noise[n];const float y=output[n+960];after+=static_cast<double>(y)*y;}
    const double db=10*std::log10(after/before);
    std::cout<<"Synthetic stationary noise change: "<<db<<" dB; 4 s mono processing wall time "<<std::chrono::duration<double>(stop-start).count()<<" s (not a Mac/DAW measurement)\n";
    // This is an activity/regression check, not a speech-quality benchmark.
    // The compact model is deliberately conservative on this white-noise
    // fixture (about -3.94 dB); do not hide that behind a stronger claim.
    require(db < -2,"trained model did not reduce the controlled noise fixture");
    p.reset();watch=true;
    for(int n=0;n<20000;++n){
        float l=noise[static_cast<std::size_t>(n)],r=l;
        if(n==321)l=std::numeric_limits<float>::quiet_NaN();
        if(n==849)l=std::numeric_limits<float>::infinity();
        p.processSample(l,r,1);
        if(!std::isfinite(l)||!std::isfinite(r)){watch=false;throw std::runtime_error("nonfinite output");}
    }
    watch=false;
    require(cppAllocations==0 && cAllocations==0,"processing allocated after prepare");
#if defined(SCULPT_TEST_C_ALLOCATION)
    std::cout<<"PASS: finite recovery, zero instrumented C++/malloc/calloc/realloc processing allocations\n";
#else
    std::cout<<"PASS: finite recovery and zero C++ processing allocations; C allocator wrapping is Linux-only\n";
#endif
}
void stereo() {
    sculptclean::VoiceDenoiser a,b;a.setAmount(.75f);b.setAmount(.75f);
    require(a.prepare(48000,2)&&b.prepare(48000,2),"stereo preparation failed");
    const auto input=fixture(24000);
    for(float x:input){float l=x,r=-.7f*x,copyL=l,copyR=r;a.processSample(l,r,2);b.processSample(copyL,copyR,2);require(l==copyL&&r==copyR,"stereo render not reproducible");require(std::isfinite(l)&&std::isfinite(r),"stereo finite failed");}
    require(a.getVoiceProbability()>=0&&a.getVoiceProbability()<=1,"VAD outsidebounds");
    std::cout<<"PASS: deterministic independent-channel stereo and bounded VAD\n";
}
}
#if defined(SCULPT_TEST_C_ALLOCATION)
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t,std::size_t);
extern "C" void* __real_realloc(void*,std::size_t);
extern "C" void* __wrap_malloc(std::size_t n){if(watch)++cAllocations;return __real_malloc(n);}
extern "C" void* __wrap_calloc(std::size_t a,std::size_t b){if(watch)++cAllocations;return __real_calloc(a,b);}
extern "C" void* __wrap_realloc(void* p,std::size_t n){if(watch)++cAllocations;return __real_realloc(p,n);}
#endif
void* operator new(std::size_t n){if(watch)++cppAllocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){try{dryAndRate();upstreamEquivalence();impulseTiming();reductionAndRealtime();stereo();return 0;}catch(const std::exception& e){watch=false;std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}

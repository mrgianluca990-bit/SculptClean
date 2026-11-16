#include "../Source/CleanEngine.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<unsigned long> allocations{0};
constexpr double pi = 3.14159265358979323846;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
using Stereo = std::array<std::vector<float>, 2>;
Stereo signal(std::size_t length) {
    Stereo s {std::vector<float>(length), std::vector<float>(length)};
    std::uint32_t seed=1234567;
    for(std::size_t i=0;i<length;++i) {
        seed=1664525U*seed+1013904223U;
        const float noise=static_cast<float>(static_cast<double>(seed)/4294967296.0-0.5)*.01f;
        s[0][i]=.11f*static_cast<float>(std::sin(2*pi*311.0*static_cast<double>(i)/48000))+noise;
        s[1][i]=-.7f*s[0][i]+.02f*static_cast<float>(std::sin(2*pi*997.0*static_cast<double>(i)/48000));
    }
    return s;
}
void run(sculptclean::CleanEngine& e, Stereo& s, int block=127, int channels=2) {
    for(std::size_t at=0;at<s[0].size();) {
        const int n=std::min(block,static_cast<int>(s[0].size()-at));
        float* ptrs[]{s[0].data()+at,s[1].data()+at};
        e.process(ptrs,channels,n); at+=static_cast<std::size_t>(n);
    }
}
sculptclean::LearnedState profile() {
    sculptclean::LearnedState p; p.noiseReady=true; p.sampleRate=48000; p.noisePower.fill(.008f);return p;
}
sculptclean::Parameters activeParameters() {
    sculptclean::Parameters p; p.denoise=.6f;p.dereverb=.3f;p.click=.6f;p.hum=.5f;
    p.sourceEnabled=true;p.sourceThresholdDb=-27;p.sourceDepth=.3f;return p;
}
void checkImmediateLearnMeters() {
    sculptclean::CleanEngine e;e.prepare(48000,128,2);
    e.restoreLearnedState(profile());
    require(e.getMeters().noiseProfileReady&&!e.getMeters().noiseLearning,
            "restored profile flags are not visible before processing");

    // The host can consume Learn in prepareToPlay while the transport is
    // stopped, then publish meters without calling process() even once.
    e.requestNoiseLearn();
    require(!e.getLearnedState().noiseReady,"Learn did not invalidate the stored profile");
    require(e.getMeters().noiseLearning,
            "Learn intent is not visible in meters before the first audio block");
    require(!e.getMeters().noiseProfileReady,
            "meters still advertise the old profile after Learn");
    require(e.getMeters().noiseLearnProgress==0,"new Learn did not publish zero progress");

    auto noise=signal(2048);run(e,noise);
    require(e.getMeters().noiseLearnProgress>0,"running Learn did not advance progress");
    e.requestNoiseLearn();
    require(e.getMeters().noiseLearning&&!e.getMeters().noiseProfileReady
                &&e.getMeters().noiseLearnProgress==0,
            "restarting Learn left stale progress or profile flags");

    // Restore and invalidation must also be observable without a callback.
    e.restoreLearnedState(profile());
    require(e.getMeters().noiseProfileReady&&!e.getMeters().noiseLearning
                &&e.getMeters().noiseLearnProgress==0,
            "profile restore did not immediately stop the pending capture");
    e.restoreLearnedState({});
    require(!e.getMeters().noiseProfileReady&&!e.getMeters().noiseLearning,
            "invalid profile restore left stale ready or learning flags");
    e.requestNoiseLearn();
    require(e.getMeters().noiseLearning&&!e.getMeters().noiseProfileReady,
            "Learn without a previous profile is not immediately visible");
    std::cout<<"PASS: immediate Learn/re-Learn and restored-profile meters without an audio callback\n";
}
void checkDelay() {
    const auto input=signal(16000);
    for(const double rate : {8000.0,44100.0,48000.0,96000.0,192000.0,384000.0})
        for(const int channels : {1,2}) {
            sculptclean::CleanEngine e; sculptclean::Parameters p;p.denoise=0;
            e.setParameters(p);e.prepare(rate,128,channels);
            require(e.getLatencySamples()==2051,"host latency changed");
            auto out=input;run(e,out,113,channels);
            for(int c=0;c<channels;++c) for(std::size_t n=0;n<out[0].size();++n)
                require(out[static_cast<std::size_t>(c)][n]==(n<2051?0:input[static_cast<std::size_t>(c)][n-2051]),"neutral delay is not bit exact");
        }
    std::cout<<"PASS: neutral 2051-sample exact delay, mono/stereo, 8-384 kHz\n";
}
void checkClick() {
    sculptclean::ClickRepair e;e.prepare(48000);e.setAmount(1);e.reset();
    double largest=0;
    for(int n=0;n<800;++n) {
        float l=(n==100||n==300||n==301)?1.0f:0.0f,r=l;e.processSample(l,r,2);
        largest=std::max(largest,std::abs(static_cast<double>(l)));
    }
    require(largest<1e-6,"isolated click not repaired");
    e.reset();double error=0;
    for(int n=0;n<10000;++n) {
        float l=.3f*static_cast<float>(std::sin(2*pi*997*n/48000)),r=l;e.processSample(l,r,2);
        if(n>50)error=std::max(error,std::abs(static_cast<double>(l)-.3f*static_cast<float>(std::sin(2*pi*997*(n-3)/48000))));
    }
    require(error<1e-7,"de-click damages clean 997 Hz tone");
    std::cout<<"PASS: single/double click removal and clean tone (max error "<<error<<")\n";
}
void checkHum() {
    for(float mains : {50.0f,60.0f}) {
        sculptclean::HumRemover e;e.prepare(48000);e.setParameters(1,mains);e.reset();
        std::array<double,4> sums{};
        for(int n=0;n<240000;++n) {
            const double a=2*pi*n/48000;
            float l=static_cast<float>(.15*std::sin(mains*a)+.1*std::sin(2*mains*a)+.1*std::sin(997*a)),r=l;
            e.processSample(l,r,2);
            if(n>=192000){sums[0]+=l*std::sin(mains*a);sums[1]+=l*std::sin(2*mains*a);sums[2]+=l*std::sin(997*a);sums[3]+=l*std::cos(997*a);}
        }
        const double fundamental=20*std::log10(std::abs(2*sums[0]/48000/.15));
        const double harmonic=20*std::log10(std::abs(2*sums[1]/48000/.1));
        const double wanted=20*std::log10(2*std::hypot(sums[2],sums[3])/48000/.1);
        require(fundamental < -20 && harmonic < -20,"hum not sufficiently attenuated");
        require(std::abs(wanted)<.1,"de-hum damages distant tone");
        e.setParameters(0,mains);for(int n=0;n<48000;++n){float l=.123f,r=-.456f;e.processSample(l,r,2);if(n>40000)require(l==.123f&&r==-.456f,"de-hum off not exact");}
        std::cout<<"PASS: "<<mains<<" Hz hum "<<fundamental<<" dB, harmonic "<<harmonic<<" dB; 997 Hz "<<wanted<<" dB\n";
    }
}
void checkBlockAndResidue() {
    auto input=signal(48000);input[0][18001]=.8f;input[1][18001]=.7f;
    auto p=activeParameters();
    sculptclean::CleanEngine a,b,r;
    a.setParameters(p);a.prepare(48000,512,2);a.restoreLearnedState(profile());
    b.setParameters(p);b.prepare(48000,512,2);b.restoreLearnedState(profile());
    p.listenRemoved=true;r.setParameters(p);r.prepare(48000,512,2);r.restoreLearnedState(profile());
    auto outA=input,outB=input,residue=input;run(a,outA,1);run(b,outB,511);run(r,residue,139);
    double error=0;for(std::size_t c=0;c<2;++c)for(std::size_t n=0;n<input[0].size();++n){
        require(outA[c][n]==outB[c][n],"block grouping changes processing");
        error=std::max(error,std::abs(static_cast<double>(outA[c][n])+residue[c][n]-(n<2051?0:input[c][n-2051])));
    }
    require(error<1e-7,"clean plus residue does not reconstruct delayed original");
    std::cout<<"PASS: block independence and Clean+Removed=delayed original (error "<<error<<")\n";
}
void checkBypassAndState() {
    sculptclean::CleanEngine e;auto p=activeParameters();p.bypass=true;p.outputGain=3;p.listenRemoved=true;
    e.setParameters(p);e.prepare(48000,128,2);e.restoreLearnedState(profile());
    const auto input=signal(48000);auto out=input;run(e,out);
    for(std::size_t c=0;c<2;++c)for(std::size_t n=0;n<out[0].size();++n)
        require(out[c][n]==(n<2051?0:input[c][n-2051]),"bypass is not aligned untrimmed original");
    require(e.getLearnedState().noiseReady,"valid noise profile missing");
    auto invalid=profile();invalid.noisePower[15]=std::numeric_limits<float>::quiet_NaN();e.restoreLearnedState(invalid);
    require(!e.getLearnedState().noiseReady,"NaN profile accepted");
    e.prepare(96000,128,2);e.restoreLearnedState(profile());require(!e.getLearnedState().noiseReady,"sample-rate-mismatched profile accepted");
    e.prepare(48000,128,2);e.requestNoiseLearn();auto noise=signal(97000);run(e,noise);
    require(e.getMeters().noiseProfileReady&&!e.getMeters().noiseLearning,"2s learn did not complete");
    std::cout<<"PASS: latency-matched bypass, profile validation and timed Learn\n";
}
void checkRealtime() {
    sculptclean::CleanEngine e;e.setParameters(activeParameters());e.prepare(48000,128,2);e.restoreLearnedState(profile());
    auto data=signal(16000);data[0][123]=std::numeric_limits<float>::infinity();data[1][801]=std::numeric_limits<float>::quiet_NaN();
    countAllocations=true;run(e,data);countAllocations=false;
    require(allocations==0,"audio path allocated C++ memory");
    for(const auto& channel:data)for(float x:channel)require(std::isfinite(x),"non-finite output");
    e.process(nullptr,0,128);
    std::cout<<"PASS: zero audio C++ allocations, finite recovery and empty-buffer safety\n";
}
}
void* operator new(std::size_t size) {if(countAllocations)++allocations;if(void* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* ptr) noexcept {std::free(ptr);}
void operator delete[](void* ptr) noexcept {std::free(ptr);}
void operator delete(void* ptr,std::size_t) noexcept {std::free(ptr);}
void operator delete[](void* ptr,std::size_t) noexcept {std::free(ptr);}
int main() {
    try {checkImmediateLearnMeters();checkDelay();checkClick();checkHum();checkBlockAndResidue();checkBypassAndState();checkRealtime();std::cout<<"All engine integration checks passed.\n";return 0;}
    catch(const std::exception& e){countAllocations=false;std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}

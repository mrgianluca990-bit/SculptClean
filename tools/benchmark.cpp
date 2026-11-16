#include "../Source/CleanEngine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {
// Match the floating-point mode used by juce::ScopedNoDenormals in the actual
// plug-in callback. The previous mode is restored for every instance/callback;
// this changes neither the timing samples retained nor the over-budget count.
class ScopedBenchmarkNoDenormals {
public:
    ScopedBenchmarkNoDenormals() noexcept {
#if defined(__SSE__) || defined(_M_X64)
        original_ = _mm_getcsr();
        volatile unsigned int enabled = original_ | 0x8040U; // x86 FTZ and DAZ.
        _mm_setcsr(enabled);
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
        __asm__ volatile("mrs %0, fpcr" : "=r"(original_));
        const std::uint64_t enabled = original_ | (std::uint64_t{1} << 24U);
        __asm__ volatile("msr fpcr, %0" : : "r"(enabled)); // AArch64 FZ.
#endif
    }
    ~ScopedBenchmarkNoDenormals() noexcept {
#if defined(__SSE__) || defined(_M_X64)
        volatile unsigned int restored = original_;
        _mm_setcsr(restored);
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
        __asm__ volatile("msr fpcr, %0" : : "r"(original_));
#endif
    }
    static const char* description() noexcept {
#if defined(__SSE__) || defined(_M_X64)
        return "Scoped x86 FTZ+DAZ, matching the plug-in callback";
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
        return "Scoped AArch64 FZ, matching the plug-in callback";
#else
        return "Unavailable on this architecture; floating-point mode unchanged";
#endif
    }
private:
#if defined(__SSE__) || defined(_M_X64)
    unsigned int original_ = 0;
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__GNUC__) || defined(__clang__))
    std::uint64_t original_ = 0;
#endif
};
constexpr double rate=48000;
constexpr int callbacks=1536;
volatile double sink=0;
struct Instance {
    sculptclean::CleanEngine engine;
    std::array<std::vector<float>,2> audio;
    explicit Instance(int block):audio{std::vector<float>(static_cast<std::size_t>(block)),std::vector<float>(static_cast<std::size_t>(block))}{}
};
std::string quoted(const std::string& input) {
    std::string output="\"";for(char c:input){if(c=='\\'||c=='\"')output+='\\';if(c=='\n')output+="\\n";else output+=c;}return output+'\"';
}
void measure(const char* mode,int count,int block,bool& first) {
    sculptclean::Parameters p;
    const std::string name=mode;
    p.denoise=name=="neutral"?0:.55f;
    if(name=="full") {p.dereverb=.35f;p.click=.45f;p.hum=.5f;p.sourceEnabled=true;p.sourceDepth=.4f;p.sourceThresholdDb=-30;}
    sculptclean::LearnedState profile;profile.noiseReady=true;profile.sampleRate=rate;profile.noisePower.fill(.005f);
    std::vector<std::unique_ptr<Instance>> instances;
    for(int i=0;i<count;++i){auto v=std::make_unique<Instance>(block);v->engine.setParameters(p);v->engine.prepare(rate,block,2);if(name!="neutral")v->engine.restoreLearnedState(profile);instances.push_back(std::move(v));}
    // Audio is created and copied outside the measured callback. Identical
    // input/timeline in separate instances is intentional: simultaneous STFT
    // boundaries expose a heavier multi-insert scheduling case.
    constexpr int sourceSize=65536;
    std::array<std::vector<float>,2> input{std::vector<float>(sourceSize),std::vector<float>(sourceSize)};
    std::uint32_t random=77;
    for(int n=0;n<sourceSize;++n){
        random=random*1664525U+1013904223U;
        const double noise=(static_cast<double>(random)/4294967296.0-.5)*.008;
        const double envelope=(n%16000)<11000?1.0:.08;
        const double phase=6.2831853071795864769*n/rate;
        input[0][static_cast<std::size_t>(n)]=static_cast<float>(envelope*(.12*std::sin(193*phase)+.03*std::sin(579*phase))+.01*std::sin(50*phase)+noise);
        input[1][static_cast<std::size_t>(n)]=static_cast<float>(envelope*(.1*std::sin(193*phase+.2)+.04*std::sin(772*phase))+.01*std::sin(50*phase)-noise);
    }
    auto fill=[&](int tick){const auto offset=static_cast<std::size_t>((tick*block)%sourceSize);for(auto& v:instances)for(std::size_t c=0;c<2;++c)for(int n=0;n<block;++n)v->audio[c][static_cast<std::size_t>(n)]=input[c][(offset+static_cast<std::size_t>(n))%sourceSize];};
    auto process=[&]{for(auto& v:instances){ScopedBenchmarkNoDenormals noDenormals;v->engine.setParameters(p);float* ptrs[]{v->audio[0].data(),v->audio[1].data()};v->engine.process(ptrs,2,block);}};
    for(int tick=0;tick<256;++tick){fill(tick);process();}
    std::vector<double> times;times.reserve(callbacks);
    double cpuSeconds=0,total=0;int over=0;
    const double budget=1e6*block/rate;
    for(int tick=0;tick<callbacks;++tick){
        fill(tick+256);
        const auto cpuStart=std::clock();const auto start=std::chrono::steady_clock::now();
        process();
        const auto finish=std::chrono::steady_clock::now();const auto cpuFinish=std::clock();
        const double elapsed=std::chrono::duration<double,std::micro>(finish-start).count();
        times.push_back(elapsed);total+=elapsed;over+=elapsed>budget?1:0;
        cpuSeconds+=static_cast<double>(cpuFinish-cpuStart)/CLOCKS_PER_SEC;
        sink+=instances.back()->audio[0][static_cast<std::size_t>(block/2)];
    }
    std::sort(times.begin(),times.end());
    if(!first)std::cout<<",\n";
    first=false;
    std::cout<<"    {\"mode\":"<<quoted(name)<<",\"instances\":"<<count<<",\"block_size\":"<<block
        <<",\"callbacks\":"<<callbacks<<",\"budget_us\":"<<budget<<",\"mean_us\":"<<total/callbacks
        <<",\"median_us\":"<<times[times.size()/2]<<",\"p99_us\":"<<times[static_cast<std::size_t>(std::ceil(times.size()*.99))-1]
        <<",\"max_us\":"<<times.back()<<",\"over_budget_callbacks\":"<<over
        <<",\"mean_budget_percent\":"<<100*total/callbacks/budget<<",\"process_cpu_percent_one_core\":"<<100*cpuSeconds/(callbacks*block/rate)<<"}";
}
}
int main(){
#if defined(__APPLE__)
    const char* backend="Apple Accelerate vDSP (radix-2 fallback if setup fails)";
#else
    const char* backend="portable C++ radix-2";
#endif
    std::cout<<std::fixed<<std::setprecision(4)<<"{\n  \"product\":\"Sculpt Clean 0.1.0\",\n  \"sample_rate\":48000,\n  \"channels_per_instance\":2,\n  \"fft_backend\":"<<quoted(backend)<<",\n  \"compiler\":"<<quoted(__VERSION__)
        <<",\n  \"denormal_control\":"<<quoted(ScopedBenchmarkNoDenormals::description())
        <<",\n  \"scope\":\"DSP core only; no host, GUI, APVTS or audio device. Serial independent instances, synchronized FFT frames. All observed wall-time outliers retained.\",\n  \"results\":[\n";
    bool first=true;for(int block:{128,512})for(const char* mode:{"neutral","denoise","full"})for(int count:{1,8,16})measure(mode,count,block,first);
    std::cout<<"\n  ]\n}\n";return std::isfinite(sink)?0:1;
}

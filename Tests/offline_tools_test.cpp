#include "../Source/OfflineTools.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
constexpr double pi=3.14159265358979323846;
using sculptclean::OfflineAudio;
void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
OfflineAudio tone(double rate,std::size_t count,double frequency,int channels=2){
    OfflineAudio audio;audio.rate=rate;audio.channels.assign(static_cast<std::size_t>(channels),std::vector<float>(count));
    for(std::size_t n=0;n<count;++n){const float value=.2f*static_cast<float>(std::sin(2*pi*frequency*static_cast<double>(n)/rate));audio.channels[0][n]=value;if(channels>1)audio.channels[1][n]=-.5f*value;}
    return audio;
}
double energy(const std::vector<float>& data,std::size_t begin,std::size_t end){double result=0;for(auto n=begin;n<end;++n)result+=static_cast<double>(data[n])*data[n];return result;}
double amplitude(const std::vector<float>& data,double rate,double frequency,std::size_t begin,std::size_t count){
    double real=0,imag=0;for(auto n=begin;n<begin+count;++n){const double phase=2*pi*frequency*static_cast<double>(n)/rate;real+=data[n]*std::cos(phase);imag+=data[n]*std::sin(phase);}return 2*std::hypot(real,imag)/static_cast<double>(count);
}
template<class F>void mustThrow(F operation,const char* message){bool caught=false;try{operation();}catch(const std::exception&){caught=true;}require(caught,message);}

void resampling(){
    auto source=tone(44100,4410,1000);
    auto up=sculptclean::offlineResample(source,48000);
    auto back=sculptclean::offlineResample(up,44100);
    require(up.size()==4800&&back.size()==source.size(),"Sample-rate conversion changed duration");
    double squaredError=0;
    for(std::size_t n=200;n<source.size()-200;++n){const double d=back.channels[0][n]-source.channels[0][n];squaredError+=d*d;require(std::abs(back.channels[1][n]+.5f*back.channels[0][n])<1e-7f,"SRC changed stereo ratio");}
    const double errorDb=10*std::log10(std::max(1e-30,squaredError)/energy(source.channels[0],200,source.size()-200));
    require(errorDb < -60,"44.1/48 kHz round trip changed wanted tone excessively");
    source.channels[0].assign(source.size(),0);source.channels[1].assign(source.size(),0);source.channels[0][2205]=1;
    back=sculptclean::offlineResample(sculptclean::offlineResample(source,48000),44100);
    const auto peak=std::max_element(back.channels[0].begin(),back.channels[0].end());
    require(static_cast<std::size_t>(peak-back.channels[0].begin())==2205,"SRC moved the impulse timestamp");
    auto high=tone(96000,9600,30000,1);auto down=sculptclean::offlineResample(high,48000);
    const double rejection=10*std::log10((energy(down.channels[0],256,down.size()-256)/static_cast<double>(down.size()-512))/(energy(high.channels[0],512,high.size()-512)/static_cast<double>(high.size()-1024)));
    require(rejection < -60,"96/48 kHz conversion aliased a 30 kHz tone");
    std::cout<<"resample_44k1_48k_roundtrip_error_db="<<errorDb<<" impulse_shift_samples=0 alias_30k_rejection_db="<<rejection<<'\n';
}

void neutralAndCoreTrim(){
    for(const std::size_t count:{1U,47U,2047U,2048U,2051U,4117U}){
        auto source=tone(48000,count,997);source.channels[0][0]=.7f;source.channels[1][count-1]=-.3f;
        sculptclean::Parameters p;p.denoise=0;
        const auto clean=sculptclean::offlineCore(source,p,{});
        require(clean.channels==source.channels,"Neutral offline core did not remove the exact pipeline latency");
        require(clean.rate==source.rate&&clean.size()==source.size(),"Core trimming changed the format");
    }
    auto source=tone(96000,997,1000);auto voice=sculptclean::offlineVoice(source,0);
    require(voice.channels==source.channels&&voice.rate==source.rate,"Voice Amount zero was not exact");
    auto attenuated=sculptclean::offlineSpectralAttenuate(source,{10,990,900,1100},0);
    require(attenuated.channels==source.channels,"Spectral zero attenuation was not exact");
    auto wet=source;for(auto& c:wet.channels)for(auto& sample:c)sample*=.1f;
    auto dry=source;sculptclean::offlineBlendSelection(dry,wet,{10,990,0,48000},0);
    require(dry.channels==source.channels,"Zero selection blend changed the dry audio");
    std::cout<<"zero_amount_and_core_latency_trim=exact\n";
}

void spectralSelection(){
    OfflineAudio source;source.rate=48000;source.channels.assign(2,std::vector<float>(16384));
    for(std::size_t n=0;n<source.size();++n){source.channels[0][n]=static_cast<float>(.2*std::sin(2*pi*1000*static_cast<double>(n)/48000)+.15*std::sin(2*pi*6000*static_cast<double>(n)/48000));source.channels[1][n]=-source.channels[0][n];}
    const sculptclean::OfflineSelection selection{4096,12288,900,1100};
    auto output=sculptclean::offlineSpectralAttenuate(source,selection,-18);
    for(std::size_t c=0;c<2;++c)for(std::size_t n=0;n<source.size();++n){
        if(n<selection.start||n>=selection.end)require(output.channels[c][n]==source.channels[c][n],"Spectral repair changed samples outside time selection");
        require(std::isfinite(output.channels[c][n]),"Spectral selection produced nonfinite output");
    }
    constexpr std::size_t begin=5796,count=4800;
    const double selected=20*std::log10(amplitude(output.channels[0],48000,1000,begin,count)/amplitude(source.channels[0],48000,1000,begin,count));
    const double outside=20*std::log10(amplitude(output.channels[0],48000,6000,begin,count)/amplitude(source.channels[0],48000,6000,begin,count));
    require(selected < -12,"Selected spectral tone was not attenuated");
    require(std::abs(outside)<.1,"Frequency outside selection changed excessively");
    for(std::size_t n=0;n<source.size();++n)require(std::abs(output.channels[0][n]+output.channels[1][n])<1e-7f,"Spectral selection changed stereo antiphase");
    std::cout<<"spectral_rectangle_selected_1k_change_db="<<selected<<" outside_6k_change_db="<<outside<<" outside_time=exact\n";
}

void selectionBlendAndCancellation(){
    OfflineAudio dry;dry.rate=48000;dry.channels.assign(1,std::vector<float>(1000,.25f));auto wet=dry;wet.channels[0].assign(1000,.75f);
    sculptclean::offlineBlendSelection(dry,wet,{100,900,0,24000},.6f);
    require(dry.channels[0][99]==.25f&&dry.channels[0][900]==.25f,"Selection blend wrote outside bounds");
    require(std::abs(dry.channels[0][500]-.55f)<1e-6f,"Selection mix amount is incorrect");
    require(dry.channels[0][100]>.25f&&dry.channels[0][100]<.254f,"Selection start was not feathered");
    require(dry.channels[0][899]>.25f&&dry.channels[0][899]<.254f,"Selection end was not feathered");
    for(std::size_t n=101;n<244;++n)require(dry.channels[0][n]>=dry.channels[0][n-1],"Selection fade is not monotonic");
    std::atomic<bool> stop{true};const auto original=dry;
    mustThrow([&]{(void)sculptclean::offlineResample(dry,44100,&stop);},"Resample ignored cancellation");
    mustThrow([&]{sculptclean::Parameters p;p.denoise=0;(void)sculptclean::offlineCore(dry,p,{},&stop);},"Core ignored cancellation");
    mustThrow([&]{(void)sculptclean::offlineSpectralAttenuate(dry,{100,900,100,2000},-12,&stop);},"Spectral selection ignored cancellation");
    require(dry.channels==original.channels,"Cancelled offline processing mutated its source");
    auto invalid=dry;invalid.channels.push_back(std::vector<float>(1));
    mustThrow([&]{(void)sculptclean::offlineResample(invalid,44100);},"Mismatched channel lengths accepted");
    std::cout<<"selection_feather_bounds_cancellation_validation=passed\n";
}

void learnedProfile(){
    OfflineAudio noise;noise.rate=48000;noise.channels.assign(1,std::vector<float>(98400));std::uint32_t random=71;
    for(auto& sample:noise.channels[0]){random=random*1664525U+1013904223U;sample=static_cast<float>((static_cast<double>(random)/4294967296.0-.5)*.02);}
    const auto profile=sculptclean::offlineLearn(noise,{0,noise.size(),0,24000});
    require(profile.noiseReady&&profile.sampleRate==48000,"Offline Learn did not return a compatible profile");
    for(float power:profile.noisePower)require(std::isfinite(power)&&power>=0,"Offline Learn returned invalid power");
    sculptclean::Parameters p;p.denoise=.8f;
    const auto clean=sculptclean::offlineCore(noise,p,profile);
    const double change=10*std::log10(energy(clean.channels[0],48000,96000)/energy(noise.channels[0],48000,96000));
    require(change < -8,"Offline learned noise was not reduced");
    auto otherRate=noise;otherRate.rate=44100;
    mustThrow([&]{(void)sculptclean::offlineCore(otherRate,p,profile);},"Mismatched learned sample rate accepted");
    mustThrow([&]{(void)sculptclean::offlineLearn(noise,{0,48000,0,24000});},"Too-short learning selection accepted");
    std::cout<<"offline_learned_noise_change_db="<<change<<" rate_and_duration_validation=passed\n";
}

void voiceMixWithOriginal(){
    auto source=tone(44100,4410,1000,1);
    for(std::size_t n=0;n<source.size();++n)source.channels[0][n]+=.03f*static_cast<float>(std::sin(2*pi*21500*static_cast<double>(n)/44100));
    const auto full=sculptclean::offlineVoice(source,1);
    const auto partial=sculptclean::offlineVoice(source,.25f);
    require(full.size()==source.size()&&partial.size()==source.size()&&partial.rate==source.rate,"RNNoise conversion changed file format");
    double maximumError=0;
    for(std::size_t n=0;n<source.size();++n){const float expected=source.channels[0][n]+.25f*(full.channels[0][n]-source.channels[0][n]);maximumError=std::max(maximumError,std::abs(static_cast<double>(partial.channels[0][n])-expected));}
    require(maximumError<1e-6,"Voice Amount does not mix against the untouched original after SRC");
    std::cout<<"voice_mix_original_after_src_max_error="<<maximumError<<'\n';
}
}
int main(){try{resampling();neutralAndCoreTrim();spectralSelection();selectionBlendAndCancellation();learnedProfile();voiceMixWithOriginal();std::cout<<"Offline tools tests passed.\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}

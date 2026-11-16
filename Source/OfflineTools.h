#pragma once
#include "CleanEngine.h"
#include "VoiceDenoiser.h"
#include <atomic>
#include <complex>
#include <functional>
#include <stdexcept>
#include <vector>

namespace sculptclean {
struct OfflineAudio {
    double rate = 48000;
    std::vector<std::vector<float>> channels;
    std::size_t size() const noexcept { return channels.empty() ? 0 : channels.front().size(); }
    bool valid() const noexcept {
        if (!std::isfinite(rate) || rate<8000 || rate>384000 || channels.empty() || channels.size()>2 || size()==0) return false;
        for(const auto& c:channels) if(c.size()!=size()) return false;
        return true;
    }
};
struct OfflineSelection { std::size_t start=0,end=0; double lowHz=0,highHz=24000; };
inline void offlineCheckCancel(const std::atomic<bool>* cancel) { if(cancel && cancel->load()) throw std::runtime_error("Operazione annullata"); }
inline void offlineRequire(const OfflineAudio& audio) { if(!audio.valid()) throw std::runtime_error("Audio mono/stereo non valido"); }

// Offline, symmetric windowed-sinc conversion. No unfiltered decimation and no
// realtime work. Output coordinates preserve the input timeline; endpoint
// kernels are normalised over available input rather than adding a delay.
inline OfflineAudio offlineResample(const OfflineAudio& source,double rate,const std::atomic<bool>* cancel=nullptr) {
    offlineRequire(source);
    if(!std::isfinite(rate)||rate<8000||rate>384000)throw std::runtime_error("Frequenza non supportata");
    if(std::abs(rate-source.rate)<.001)return source;
    OfflineAudio out;out.rate=rate;
    const auto length=static_cast<std::size_t>(std::max(1.0,std::round(source.size()*rate/source.rate)));
    out.channels.assign(source.channels.size(),std::vector<float>(length));
    constexpr double pi=3.14159265358979323846;
    const double cutoff=std::min(1.0,rate/source.rate)*.94;
    const int radius=static_cast<int>(std::ceil(32.0/std::min(1.0,rate/source.rate)));
    for(std::size_t n=0;n<length;++n){
        if(n%4096==0)offlineCheckCancel(cancel);
        const double position=n*source.rate/rate;
        const auto centre=static_cast<std::int64_t>(std::floor(position));
        std::array<double,2> sum{};double weights=0;
        for(int k=-radius;k<=radius;++k){
            const auto index=centre+k;
            if(index<0||index>=static_cast<std::int64_t>(source.size()))continue;
            const double distance=position-index;
            if(std::abs(distance)>radius)continue;
            const double v=pi*cutoff*distance;
            const double sinc=std::abs(v)<1e-12?1.0:std::sin(v)/v;
            const double window=.42+.5*std::cos(pi*distance/radius)+.08*std::cos(2*pi*distance/radius);
            const double weight=cutoff*sinc*window;weights+=weight;
            for(std::size_t c=0;c<source.channels.size();++c)sum[c]+=source.channels[c][static_cast<std::size_t>(index)]*weight;
        }
        for(std::size_t c=0;c<out.channels.size();++c)out.channels[c][n]=static_cast<float>(sum[c]/std::max(1e-12,weights));
    }
    return out;
}
inline OfflineAudio offlineVoice(const OfflineAudio& source,float amount,const std::atomic<bool>* cancel=nullptr,std::atomic<float>* progress=nullptr) {
    offlineRequire(source);if(amount<=0)return source;
    auto work=offlineResample(source,48000,cancel);
    VoiceDenoiser voice;voice.setAmount(1.0f);
    if(!voice.prepare(48000,static_cast<int>(work.channels.size())))throw std::runtime_error("RNNoise non disponibile");
    auto output=work;const auto delay=static_cast<std::size_t>(voice.getLatencySamples());
    for(std::size_t n=0;n<work.size()+delay;++n){
        if(n%4096==0){offlineCheckCancel(cancel);if(progress)progress->store(static_cast<float>(n)/static_cast<float>(work.size()+delay));}
        float l=n<work.size()?work.channels[0][n]:0;
        float r=work.channels.size()>1?(n<work.size()?work.channels[1][n]:0):l;
        voice.processSample(l,r,static_cast<int>(work.channels.size()));
        if(n>=delay){output.channels[0][n-delay]=l;if(output.channels.size()>1)output.channels[1][n-delay]=r;}
    }
    auto result=offlineResample(output,source.rate,cancel);
    const float blend=std::clamp(amount,0.0f,1.0f);
    for(std::size_t c=0;c<result.channels.size();++c){
        result.channels[c].resize(source.size(),0);
        for(std::size_t n=0;n<source.size();++n)
            result.channels[c][n]=source.channels[c][n]+blend*(result.channels[c][n]-source.channels[c][n]);
    }
    return result;
}
inline LearnedState offlineLearn(const OfflineAudio& source,OfflineSelection selection,const std::atomic<bool>* cancel=nullptr) {
    offlineRequire(source);selection.end=std::min(selection.end,source.size());
    const auto minimum=static_cast<std::size_t>(std::ceil(source.rate*2))+SpectralCleaner::hopSize;
    if(selection.end<=selection.start || selection.end-selection.start<minimum)throw std::runtime_error("Seleziona almeno 2,1 secondi di solo rumore");
    SpectralCleaner cleaner;cleaner.prepare(source.rate,static_cast<int>(source.channels.size()));cleaner.requestNoiseLearn();
    for(std::size_t n=selection.start;n<selection.end && !cleaner.getMeters().noiseProfileReady;++n){
        if(n%4096==0)offlineCheckCancel(cancel);
        float l=source.channels[0][n],r=source.channels.size()>1?source.channels[1][n]:l;
        cleaner.processSample(l,r,static_cast<int>(source.channels.size()));
    }
    if(!cleaner.getMeters().noiseProfileReady)throw std::runtime_error("Profilo rumore non completato");
    return cleaner.getLearnedState();
}
inline OfflineAudio offlineCore(const OfflineAudio& source,Parameters parameters,const LearnedState& profile,const std::atomic<bool>* cancel=nullptr,std::atomic<float>* progress=nullptr) {
    offlineRequire(source);CleanEngine engine;engine.setParameters(parameters);engine.prepare(source.rate,512,static_cast<int>(source.channels.size()));engine.restoreLearnedState(profile);
    if(parameters.denoise>0&&!engine.getLearnedState().noiseReady)throw std::runtime_error("Apprendi prima il rumore dalla selezione");
    auto out=source;std::array<std::array<float,512>,2> block{};
    const std::size_t delay=static_cast<std::size_t>(engine.getLatencySamples());
    for(std::size_t at=0;at<source.size()+delay;at+=512){
        offlineCheckCancel(cancel);const auto count=std::min<std::size_t>(512,source.size()+delay-at);
        for(std::size_t c=0;c<source.channels.size();++c)for(std::size_t n=0;n<count;++n)block[c][n]=at+n<source.size()?source.channels[c][at+n]:0;
        float* pointers[]{block[0].data(),block[1].data()};engine.process(pointers,static_cast<int>(source.channels.size()),static_cast<int>(count));
        for(std::size_t n=0;n<count;++n)if(at+n>=delay)for(std::size_t c=0;c<source.channels.size();++c)out.channels[c][at+n-delay]=block[c][n];
        if(progress)progress->store(static_cast<float>(at+count)/static_cast<float>(source.size()+delay));
    }
    return out;
}
inline void offlineBlendSelection(OfflineAudio& destination,const OfflineAudio& wet,OfflineSelection selection,float amount=1.0f) {
    offlineRequire(destination);offlineRequire(wet);
    if(wet.size()!=destination.size()||wet.channels.size()!=destination.channels.size()||wet.rate!=destination.rate)throw std::runtime_error("Il risultato non ha la stessa durata o frequenza dell'originale");
    selection.end=std::min(selection.end,destination.size());if(selection.start>=selection.end)return;
    const auto edge=std::min<std::size_t>(static_cast<std::size_t>(destination.rate*.003),(selection.end-selection.start)/4);
    for(std::size_t n=selection.start;n<selection.end;++n){
        double mix=std::clamp(amount,0.0f,1.0f);
        if(edge>0){const auto distance=std::min(n-selection.start,selection.end-1-n);mix*=std::min(1.0,static_cast<double>(distance+1)/edge);}
        for(std::size_t c=0;c<destination.channels.size();++c)destination.channels[c][n]+=static_cast<float>(mix*(wet.channels[c][n]-destination.channels[c][n]));
    }
}
namespace offline_detail {
inline void fft(std::vector<std::complex<double>>& data,bool inverse){
    const auto n=data.size();for(std::size_t i=1,j=0;i<n;++i){auto bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(data[i],data[j]);}
    constexpr double pi=3.14159265358979323846;
    for(std::size_t length=2;length<=n;length*=2){const double angle=(inverse?2:-2)*pi/static_cast<double>(length);const std::complex<double> step(std::cos(angle),std::sin(angle));for(std::size_t at=0;at<n;at+=length){std::complex<double> factor(1,0);for(std::size_t k=0;k<length/2;++k){const auto a=data[at+k],b=data[at+k+length/2]*factor;data[at+k]=a+b;data[at+k+length/2]=a-b;factor*=step;}}}
    if(inverse)for(auto& v:data)v/=static_cast<double>(n);
}
}
// Rectangle attenuation with time/frequency feathering. This removes selected
// spectral content; it does not claim to recreate missing speech harmonics.
inline OfflineAudio offlineSpectralAttenuate(const OfflineAudio& source,OfflineSelection selection,float depthDb,const std::atomic<bool>* cancel=nullptr) {
    offlineRequire(source);selection.end=std::min(selection.end,source.size());if(depthDb>=0||selection.start>=selection.end)return source;
    constexpr int fftSize=2048,hop=512;constexpr double pi=3.14159265358979323846;
    auto output=source;const double gain=std::pow(10.0,std::clamp(static_cast<double>(depthDb),-90.0,0.0)/20);
    std::array<double,fftSize> window{};for(int i=0;i<fftSize;++i)window[static_cast<std::size_t>(i)]=std::sqrt(.5-.5*std::cos(2*pi*i/fftSize));
    std::vector<std::complex<double>> spectrum(fftSize);
    for(std::size_t c=0;c<source.channels.size();++c){
        std::vector<double> correction(source.size(),0);
        const auto first=(static_cast<std::int64_t>(selection.start)/hop)*hop-fftSize;
        const auto last=static_cast<std::int64_t>(selection.end)+fftSize;
        for(std::int64_t frame=first;frame<last;frame+=hop){
            offlineCheckCancel(cancel);
            const double centre=frame+fftSize*.5;
            double coverage=std::clamp((centre-static_cast<double>(selection.start)+hop)/hop,0.0,1.0)*std::clamp((static_cast<double>(selection.end)-centre+hop)/hop,0.0,1.0);
            if(coverage==0)continue;
            for(int i=0;i<fftSize;++i){const auto index=frame+i;spectrum[static_cast<std::size_t>(i)]={index>=0&&index<static_cast<std::int64_t>(source.size())?source.channels[c][static_cast<std::size_t>(index)]*window[static_cast<std::size_t>(i)]:0,0};}
            offline_detail::fft(spectrum,false);
            for(int k=0;k<=fftSize/2;++k){
                const double hz=static_cast<double>(k)*source.rate/fftSize,feather=2*source.rate/fftSize;
                const double frequency=std::clamp((hz-selection.lowHz+feather)/feather,0.0,1.0)*std::clamp((selection.highHz-hz+feather)/feather,0.0,1.0);
                spectrum[static_cast<std::size_t>(k)]*=coverage*frequency*(gain-1);
                if(k>0&&k<fftSize/2)spectrum[static_cast<std::size_t>(fftSize-k)]=std::conj(spectrum[static_cast<std::size_t>(k)]);
            }
            offline_detail::fft(spectrum,true);
            for(int i=0;i<fftSize;++i){const auto index=frame+i;if(index>=static_cast<std::int64_t>(selection.start)&&index<static_cast<std::int64_t>(selection.end))correction[static_cast<std::size_t>(index)]+=.5*spectrum[static_cast<std::size_t>(i)].real()*window[static_cast<std::size_t>(i)];}
        }
        const auto ramp=std::min<std::size_t>(static_cast<std::size_t>(source.rate*.003),(selection.end-selection.start)/4);
        for(std::size_t n=selection.start;n<selection.end;++n){const double edge=ramp?std::min(1.0,static_cast<double>(std::min(n-selection.start+1,selection.end-n))/ramp):1;output.channels[c][n]+=static_cast<float>(correction[n]*edge);}
    }
    return output;
}
} // namespace sculptclean

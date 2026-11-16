#pragma once
#include "SpectralCleaner.h"
#include "ClickRepair.h"
#include "HumRemover.h"
#include "SourceExtractor.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace sculptclean {
struct Parameters {
    float denoise=.35f, dereverb=0.0f, click=0.0f, hum=0.0f, humHz=50.0f;
    float sourceThresholdDb=-32.0f, sourceDepth=.5f, outputGain=1.0f;
    bool sourceEnabled=false, listenRemoved=false, bypass=false;
};
struct Meters {
    float inputPeak=0, outputPeak=0, denoiseReductionDb=0, dereverbReductionDb=0;
    float sourceReductionDb=0, noiseLearnProgress=0;
    bool noiseLearning=false, noiseProfileReady=false;
};
using LearnedState = SpectralCleaner::LearnedState;

class CleanEngine {
public:
    static constexpr int latencySamples = 2048 + ClickRepair::latencySamples;
    CleanEngine() { prepare(48000.0, 512, 2); }
    void prepare(double rate, int maximumBlockSize, int channels) {
        (void)maximumBlockSize;
        rate_ = std::isfinite(rate) && rate >= 8000.0 && rate <= 384000.0 ? rate : 48000.0;
        channelCount_ = std::clamp(channels, 1, 2);
        gainStep_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.015 * rate_)));
        fadeStep_ = static_cast<float>(1.0 / (0.005 * rate_));
        neutralWaitSamples_ = static_cast<int>(rate_ * 0.5) + latencySamples;
        hum_.prepare(rate_); click_.prepare(rate_); spectral_.prepare(rate_, channelCount_);
        source_.prepare(rate_);
        reset();
    }
    void reset() {
        dry_ = {}; position_ = 0; meters_ = {};
        currentOutput_ = target_.outputGain;
        wet_ = target_.bypass ? 0.0f : 1.0f;
        listen_ = target_.listenRemoved ? 1.0f : 0.0f;
        updateStages(); hum_.reset(); click_.reset(); spectral_.reset(); source_.reset();
        neutral_ = targetsNeutral(); neutralRemaining_ = neutral_ ? 0 : neutralWaitSamples_;
    }
    void setParameters(const Parameters& p) {
        assign(target_.denoise,p.denoise,0,1); assign(target_.dereverb,p.dereverb,0,1);
        assign(target_.click,p.click,0,1); assign(target_.hum,p.hum,0,1);
        assign(target_.humHz,p.humHz,50,60);
        assign(target_.sourceThresholdDb,p.sourceThresholdDb,-60,0);
        assign(target_.sourceDepth,p.sourceDepth,0,1); assign(target_.outputGain,p.outputGain,0,4);
        target_.sourceEnabled=p.sourceEnabled; target_.listenRemoved=p.listenRemoved; target_.bypass=p.bypass;
        updateStages();
        const bool nowNeutral=targetsNeutral();
        if (nowNeutral != neutral_) neutralRemaining_=neutralWaitSamples_;
        neutral_=nowNeutral;
    }
    void requestNoiseLearn() { spectral_.requestNoiseLearn(); }
    int getLatencySamples() const noexcept { return latencySamples; }
    const Meters& getMeters() const noexcept { return meters_; }
    LearnedState getLearnedState() const { return spectral_.getLearnedState(); }
    void restoreLearnedState(const LearnedState& state) { spectral_.restoreLearnedState(state); updateMeters(); }
    void process(float* const* audio, int channels, int count) {
        meters_.inputPeak=meters_.outputPeak=0;
        if (!audio || channels<1 || !audio[0] || count<=0) { updateMeters(); return; }
        const int active=channels>1 && audio[1] ? 2 : 1;
        for (int n=0;n<count;++n) {
            const float rawL=finite(audio[0][n]), rawR=active>1?finite(audio[1][n]):rawL;
            meters_.inputPeak=std::max({meters_.inputPeak,std::abs(rawL),std::abs(rawR)});
            const float delayedL=dry_[0][position_], delayedR=dry_[1][position_];
            dry_[0][position_]=rawL; dry_[1][position_]=rawR;
            position_=(position_+1)%latencySamples;
            float cleanL=rawL,cleanR=rawR;
            hum_.processSample(cleanL,cleanR,active);
            click_.processSample(cleanL,cleanR,active);
            spectral_.processSample(cleanL,cleanR,active);
            source_.processSample(cleanL,cleanR,delayedL,delayedR,active);
            if (neutral_ && neutralRemaining_>0) --neutralRemaining_;
            if (neutral_ && neutralRemaining_==0) { cleanL=delayedL; cleanR=delayedR; }
            currentOutput_+=gainStep_*(target_.outputGain-currentOutput_);
            if(std::abs(currentOutput_-target_.outputGain)<1.0e-7f)currentOutput_=target_.outputGain;
            wet_+=std::clamp((target_.bypass?0.0f:1.0f)-wet_,-fadeStep_,fadeStep_);
            listen_+=std::clamp((target_.listenRemoved?1.0f:0.0f)-listen_,-fadeStep_,fadeStep_);
            const double listenL=static_cast<double>(cleanL)+listen_*(static_cast<double>(delayedL)-2.0*cleanL);
            const double listenR=static_cast<double>(cleanR)+listen_*(static_cast<double>(delayedR)-2.0*cleanR);
            const float wetL=finite(static_cast<float>(listenL*currentOutput_));
            const float wetR=finite(static_cast<float>(listenR*currentOutput_));
            const float outL=wet_==0?delayedL:(wet_==1?wetL:finite(delayedL+wet_*(wetL-delayedL)));
            const float outR=wet_==0?delayedR:(wet_==1?wetR:finite(delayedR+wet_*(wetR-delayedR)));
            audio[0][n]=outL; if(active>1)audio[1][n]=outR;
            meters_.outputPeak=std::max({meters_.outputPeak,std::abs(outL),std::abs(outR)});
        }
        updateMeters();
    }
private:
    static float finite(float x) noexcept { return std::isfinite(x) ? x : 0.0f; }
    static void assign(float&to,float value,float lo,float hi) noexcept {if(std::isfinite(value))to=std::clamp(value,lo,hi);}
    bool targetsNeutral() const noexcept {return target_.denoise==0&&target_.dereverb==0&&target_.hum==0&&target_.click==0&&(!target_.sourceEnabled||target_.sourceDepth==0);}
    void updateStages() {
        hum_.setParameters(target_.hum,target_.humHz); click_.setAmount(target_.click);
        spectral_.setParameters(target_.denoise,target_.dereverb);
        source_.setParameters(target_.sourceThresholdDb,target_.sourceDepth,target_.sourceEnabled);
    }
    void updateMeters() {
        const auto& s=spectral_.getMeters();
        meters_.noiseLearning=s.noiseLearning; meters_.noiseProfileReady=s.noiseProfileReady;
        meters_.noiseLearnProgress=s.noiseLearnProgress;
        meters_.denoiseReductionDb=wet_*s.denoiseReductionDb;
        meters_.dereverbReductionDb=wet_*s.dereverbReductionDb;
        meters_.sourceReductionDb=wet_*source_.getReductionDb();
    }
    Parameters target_{}; Meters meters_{};
    SpectralCleaner spectral_; HumRemover hum_; ClickRepair click_; SourceExtractor source_;
    std::array<std::array<float,latencySamples>,2> dry_{};
    std::size_t position_=0;
    double rate_=48000;
    int channelCount_=2,neutralWaitSamples_=26051,neutralRemaining_=0;
    float currentOutput_=1,wet_=1,listen_=0,gainStep_=.001f,fadeStep_=.004f;
    bool neutral_=false;
};
} // namespace sculptclean

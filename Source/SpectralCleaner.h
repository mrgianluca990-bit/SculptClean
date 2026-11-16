#pragma once

#include "SpectralDereverb.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace sculptclean {

// One shared STFT for the two spectral restoration stages. The estimator uses
// the decision-directed a-priori SNR idea, followed by a bounded Wiener gain;
// it is not the full Ephraim-Malah MMSE-STSA algorithm or a neural separator.
// Background: https://webee.technion.ac.il/Sites/People/IsraelCohen/Publications/icassp04.pdf
// FFT backend on macOS: Apple's in-place complex FFT, unnormalised both ways.
// https://developer.apple.com/documentation/accelerate/vdsp_fft_zip
class SpectralCleaner {
public:
    static constexpr std::size_t fftSize = 2048;
    static constexpr std::size_t hopSize = 512;
    static constexpr std::size_t binCount = fftSize / 2 + 1;

    struct LearnedState {
        std::array<float, binCount> noisePower{};
        bool noiseReady = false;
        double sampleRate = 48000.0;
    };

    struct Meters {
        float denoiseReductionDb = 0.0f;
        float dereverbReductionDb = 0.0f;
        bool noiseLearning = false;
        bool noiseProfileReady = false;
        float noiseLearnProgress = 0.0f;
    };

    SpectralCleaner() noexcept { prepare(48000.0, 2); }
    ~SpectralCleaner() noexcept {
#if defined(__APPLE__)
        if (nativeFft_ != nullptr) vDSP_destroy_fftsetup(nativeFft_);
#endif
    }
    SpectralCleaner(const SpectralCleaner&) = delete;
    SpectralCleaner& operator=(const SpectralCleaner&) = delete;

    // Only lifecycle code calls prepare(); all buffers are fixed-size members.
    // Apple FFT plan creation is confined to this method, never processSample().
    void prepare(double sampleRate, int channels) noexcept {
        rate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
            ? sampleRate : 48000.0;
        channels_ = std::clamp(channels, 1, 2);
        constexpr double pi = 3.1415926535897932384626433832795;
        for (std::size_t i = 0; i < fftSize; ++i) {
            window_[i] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / fftSize)));
            std::size_t value = i, reversed = 0;
            for (unsigned bit = 0; bit < fftOrder_; ++bit) { reversed = (reversed << 1U) | (value & 1U); value >>= 1U; }
            bitReverse_[i] = static_cast<std::uint16_t>(reversed);
        }
        for (std::size_t i = 0; i < twiddles_.size(); ++i) {
            const double phase = -2.0 * pi * static_cast<double>(i) / fftSize;
            twiddles_[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        }
#if defined(__APPLE__)
        if (nativeFft_ == nullptr) nativeFft_ = vDSP_create_fftsetup(fftOrder_, kFFTRadix2);
#endif
        const double frameSeconds = static_cast<double>(hopSize) / rate_;
        parameterCoefficient_ = static_cast<float>(std::exp(-frameSeconds / 0.030));
        closeCoefficient_ = static_cast<float>(std::exp(-frameSeconds / 0.080));
        openCoefficient_ = static_cast<float>(std::exp(-frameSeconds / 0.006));
        priorCoefficient_ = static_cast<float>(std::exp(-frameSeconds / 0.170));
        learnTotal_ = static_cast<std::uint64_t>(std::llround(rate_ * 2.0));
        dereverb_.prepare(rate_, static_cast<int>(fftSize), static_cast<int>(hopSize));
        reset();
    }

    void reset() noexcept {
        input_ = {}; correction_ = {}; fft_ = {}; original_ = {};
        power_ = {}; previousPower_ = {}; previousCleanPower_ = {};
        noisePower_ = {}; noiseAccum_ = {};
        gain_.fill(1.0f); gainTarget_.fill(1.0f);
        position_ = hopCounter_ = 0;
        currentDenoise_ = bypass_ ? 0.0f : targetDenoise_;
        learning_ = learned_ = false;
        learnElapsed_ = learnedFrames_ = 0;
        denoiseReductionDb_ = 0.0f;
        denoiseDormant_ = true;
        dereverb_.reset();
        dereverb_.setAmount(bypass_ ? 0.0f : targetDereverb_);
    }

    void setParameters(float denoise, float dereverb, bool bypassSpectral = false) noexcept {
        if (std::isfinite(denoise)) targetDenoise_ = std::clamp(denoise, 0.0f, 1.0f);
        if (std::isfinite(dereverb)) targetDereverb_ = std::clamp(dereverb, 0.0f, 1.0f);
        bypass_ = bypassSpectral;
        dereverb_.setAmount(bypass_ ? 0.0f : targetDereverb_);
    }

    int getLatencySamples() const noexcept { return static_cast<int>(fftSize); }
    Meters getMeters() const noexcept {
        Meters result;
        result.denoiseReductionDb = denoiseReductionDb_;
        result.dereverbReductionDb = dereverb_.getReductionDb();
        result.noiseLearning = learning_;
        result.noiseProfileReady = learned_;
        result.noiseLearnProgress = learning_
            ? std::min(1.0f, static_cast<float>(learnElapsed_) / static_cast<float>(learnTotal_)) : 0.0f;
        return result;
    }

    // Ignore windows containing pre-Learn audio. The capture completes at the
    // first hop boundary at/after two seconds (at most one hop later).
    void requestNoiseLearn() noexcept {
        noiseAccum_ = {};
        learnElapsed_ = learnedFrames_ = 0;
        learning_ = true;
        learned_ = false;
    }

    LearnedState getLearnedState() const noexcept {
        LearnedState result;
        result.noisePower = noisePower_;
        result.noiseReady = learned_;
        result.sampleRate = rate_;
        return result;
    }

    void restoreLearnedState(const LearnedState& state) noexcept {
        bool valid = state.noiseReady && std::isfinite(state.sampleRate)
            && std::abs(state.sampleRate - rate_) <= 0.5;
        for (float power : state.noisePower) valid = valid && std::isfinite(power) && power >= 0.0f;
        learned_ = valid;
        learning_ = false;
        learnElapsed_ = learnedFrames_ = 0;
        noiseAccum_ = {};
        noisePower_ = {};
        if (valid) noisePower_ = state.noisePower;
        previousCleanPower_ = {};
    }

    // Exactly fftSize samples of streaming delay. The unmodified delayed input
    // is the base signal; overlap-add contains only spectral corrections. Thus
    // a settled neutral stage is bit-exact, without FFT reconstruction colour.
    void processSample(float& left, float& right, int channels) noexcept {
        const int activeChannels = std::clamp(channels, 1, channels_);
        const float rawL = finiteAudio(left);
        const float rawR = activeChannels == 2 ? finiteAudio(right) : rawL;
        const float delayedL = input_[0][position_];
        const float delayedR = input_[1][position_];
        left = finiteAudio(static_cast<double>(delayedL) + correction_[0][position_]);
        right = activeChannels == 2
            ? finiteAudio(static_cast<double>(delayedR) + correction_[1][position_]) : left;
        correction_[0][position_] = correction_[1][position_] = 0.0f;
        input_[0][position_] = rawL;
        input_[1][position_] = rawR;
        position_ = (position_ + 1U) & mask_;
        if (learning_) ++learnElapsed_;
        if (++hopCounter_ == hopSize) {
            hopCounter_ = 0;
            processFrame(activeChannels);
        }
    }

private:
    static constexpr unsigned fftOrder_ = 11;
    static constexpr std::size_t mask_ = fftSize - 1;
    static float finiteAudio(double value) noexcept {
        // The bound is far outside audio headroom and prevents overflow inside
        // float FFTs when a host feeds a corrupt yet finite near-FLT_MAX value.
        return std::isfinite(value) ? static_cast<float>(std::clamp(value, -1.0e6, 1.0e6)) : 0.0f;
    }
    static void smooth(float& current, float target, float coefficient) noexcept {
        current = target + coefficient * (current - target);
        if (std::abs(current - target) < 1.0e-7f) current = target;
    }

    void transform(std::array<std::complex<float>, fftSize>& data, bool inverse) noexcept {
#if defined(__APPLE__)
        if (nativeFft_ != nullptr) {
            auto* floats = reinterpret_cast<float*>(data.data());
            DSPSplitComplex split {floats, floats + 1};
            vDSP_fft_zip(nativeFft_, &split, 2, fftOrder_, inverse ? kFFTDirection_Inverse : kFFTDirection_Forward);
            return;
        }
#endif
        for (std::size_t i = 0; i < fftSize; ++i) {
            const std::size_t target = bitReverse_[i];
            if (target > i) std::swap(data[i], data[target]);
        }
        for (std::size_t length = 2; length <= fftSize; length *= 2) {
            const std::size_t half = length / 2, step = fftSize / length;
            for (std::size_t start = 0; start < fftSize; start += length) {
                for (std::size_t k = 0; k < half; ++k) {
                    auto twiddle = twiddles_[k * step];
                    if (inverse) twiddle = std::conj(twiddle);
                    const auto even = data[start + k];
                    const auto odd = data[start + k + half] * twiddle;
                    data[start + k] = even + odd;
                    data[start + k + half] = even - odd;
                }
            }
        }
    }

    void processFrame(int channels) noexcept {
        smooth(currentDenoise_, bypass_ ? 0.0f : targetDenoise_, parameterCoefficient_);
        // The delayed dry signal and input ring keep running while idle. Only
        // the correction analysis sleeps, so re-enabling needs no empty FFT
        // history and cannot open a gap in the audio. Existing corrections
        // are still drained by processSample() during the transition.
        if (!learning_ && !dereverb_.isActive() && denoiseDormant_
            && (!learned_ || currentDenoise_ == 0.0f)) {
            denoiseReductionDb_ = 0.0f;
            return;
        }
        for (int channel = 0; channel < channels; ++channel) {
            auto& spectrum = fft_[static_cast<std::size_t>(channel)];
            const auto& input = input_[static_cast<std::size_t>(channel)];
            for (std::size_t i = 0; i < fftSize; ++i)
                spectrum[i] = {input[(position_ + i) & mask_] * window_[i], 0.0f};
            transform(spectrum, false);
            for (std::size_t bin = 0; bin < binCount; ++bin)
                original_[static_cast<std::size_t>(channel)][bin] = spectrum[bin];
        }
        if (channels == 1) {
            for (std::size_t bin = 0; bin < binCount; ++bin) {
                fft_[1][bin] = fft_[0][bin];
                original_[1][bin] = original_[0][bin];
            }
        }
        for (std::size_t bin = 0; bin < binCount; ++bin) {
            power_[bin] = 0.5f * (std::norm(original_[0][bin]) + std::norm(original_[1][bin]));
            if (learning_ && learnElapsed_ >= fftSize) noiseAccum_[bin] += power_[bin];
        }
        if (learning_ && learnElapsed_ >= fftSize) {
            ++learnedFrames_;
            if (learnElapsed_ >= learnTotal_) {
                for (std::size_t bin = 0; bin < binCount; ++bin)
                    noisePower_[bin] = static_cast<float>(noiseAccum_[bin] / static_cast<double>(learnedFrames_));
                learning_ = false;
                learned_ = true;
                previousCleanPower_ = {};
            }
        }

        const float minimumGain = std::pow(10.0f, -1.2f * currentDenoise_); // max 24 dB
        for (std::size_t bin = 0; bin < binCount; ++bin) {
            float target = 1.0f;
            if (learned_ && currentDenoise_ > 0.0f) {
                const std::size_t previous = bin == 0 ? 0 : bin - 1;
                const std::size_t next = std::min(bin + 1, binCount - 1);
                const float localPower = 0.25f * power_[previous] + 0.5f * power_[bin] + 0.25f * power_[next];
                const float localNoise = std::max(1.0e-18f,
                    0.25f * noisePower_[previous] + 0.5f * noisePower_[bin] + 0.25f * noisePower_[next]);
                const float posterior = std::min(1.0e8f, localPower / localNoise);
                float prior = priorCoefficient_ * std::min(1.0e8f, previousCleanPower_[bin] / localNoise)
                    + (1.0f - priorCoefficient_) * std::max(0.0f, posterior - 1.0f);
                // A clearly dominant new event must not wait for the slow
                // decision-directed prior. No voice classifier is implied.
                const bool onset = posterior > 4.0f && localPower > previousPower_[bin] * 2.5f;
                if (onset) prior = std::max(prior, posterior - 1.0f);
                const float estimatorGain = prior / (1.0f + prior);
                previousCleanPower_[bin] = localPower * estimatorGain * estimatorGain;
                target = std::max(minimumGain, prior / (prior + currentDenoise_));
                if (onset) target = std::max(target, 0.95f);
                previousPower_[bin] = localPower;
            } else {
                previousPower_[bin] = power_[bin];
                previousCleanPower_[bin] = 0.0f;
            }
            gainTarget_[bin] = target;
        }

        dereverb_.processFrame(fft_[0].data(), fft_[1].data(), static_cast<int>(binCount), channels);
        double powerBefore = 0.0, powerAfter = 0.0;
        bool correctionsExist = false;
        denoiseDormant_ = true;
        for (std::size_t bin = 0; bin < binCount; ++bin) {
            const std::size_t previous = bin == 0 ? 0 : bin - 1;
            const std::size_t next = std::min(bin + 1, binCount - 1);
            const float frequencySmoothed = 0.25f * gainTarget_[previous]
                + 0.5f * gainTarget_[bin] + 0.25f * gainTarget_[next];
            const float desiredGain = std::max(frequencySmoothed, 0.95f * gainTarget_[bin]);
            smooth(gain_[bin], desiredGain, desiredGain > gain_[bin] ? openCoefficient_ : closeCoefficient_);
            denoiseDormant_ = denoiseDormant_ && gain_[bin] == 1.0f;
            const double binPower = channels == 1 ? static_cast<double>(std::norm(fft_[0][bin]))
                : 0.5 * (std::norm(fft_[0][bin]) + std::norm(fft_[1][bin]));
            const double weight = bin == 0 || bin == binCount - 1 ? 1.0 : 2.0;
            powerBefore += weight * binPower;
            powerAfter += weight * binPower * gain_[bin] * gain_[bin];
            for (int channel = 0; channel < channels; ++channel) {
                auto& sample = fft_[static_cast<std::size_t>(channel)][bin];
                sample = sample * gain_[bin] - original_[static_cast<std::size_t>(channel)][bin];
                correctionsExist = correctionsExist || sample.real() != 0.0f || sample.imag() != 0.0f;
            }
        }
        denoiseReductionDb_ = powerBefore > 1.0e-20
            ? static_cast<float>(std::clamp(-10.0 * std::log10(std::max(1.0e-12, powerAfter / powerBefore)), 0.0, 24.0)) : 0.0f;
        if (!correctionsExist) return;

        // sqrt-Hann analysis/synthesis, four overlaps: sum(window^2)=2.
        // Inverse FFT is unnormalised, so the synthesis scale is 1/(2*N).
        constexpr float synthesisScale = 0.5f / static_cast<float>(fftSize);
        for (int channel = 0; channel < channels; ++channel) {
            auto& spectrum = fft_[static_cast<std::size_t>(channel)];
            spectrum[0] = {spectrum[0].real(), 0.0f};
            spectrum[binCount - 1] = {spectrum[binCount - 1].real(), 0.0f};
            for (std::size_t bin = 1; bin + 1 < binCount; ++bin)
                spectrum[fftSize - bin] = std::conj(spectrum[bin]);
            transform(spectrum, true);
            auto& output = correction_[static_cast<std::size_t>(channel)];
            for (std::size_t i = 0; i < fftSize; ++i)
                output[(position_ + i) & mask_] += spectrum[i].real() * window_[i] * synthesisScale;
        }
    }

    double rate_ = 48000.0;
    int channels_ = 2;
    std::size_t position_ = 0, hopCounter_ = 0;
    std::array<std::array<float, fftSize>, 2> input_{}, correction_{};
    std::array<std::array<std::complex<float>, fftSize>, 2> fft_{};
    std::array<std::array<std::complex<float>, binCount>, 2> original_{};
    std::array<float, fftSize> window_{};
    std::array<std::uint16_t, fftSize> bitReverse_{};
    std::array<std::complex<float>, fftSize / 2> twiddles_{};
    std::array<float, binCount> power_{}, previousPower_{}, previousCleanPower_{};
    std::array<float, binCount> noisePower_{}, gain_{}, gainTarget_{};
    std::array<double, binCount> noiseAccum_{};
    float targetDenoise_ = 0.0f, currentDenoise_ = 0.0f, targetDereverb_ = 0.0f;
    float parameterCoefficient_ = 0.0f, openCoefficient_ = 0.0f, closeCoefficient_ = 0.0f, priorCoefficient_ = 0.0f;
    float denoiseReductionDb_ = 0.0f;
    bool bypass_ = false, learning_ = false, learned_ = false, denoiseDormant_ = true;
    std::uint64_t learnTotal_ = 96000, learnElapsed_ = 0, learnedFrames_ = 0;
    SpectralDereverb dereverb_;
#if defined(__APPLE__)
    FFTSetup nativeFft_ = nullptr;
#endif
};

} // namespace sculptclean

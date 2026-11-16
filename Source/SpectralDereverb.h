#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace sculptclean {

// Low-cost late-reverberation suppression in an existing STFT. The delayed
// spectral-power estimate follows the exponential late-room model described
// by Lebart, Boucher & Denbigh (2001), with conservative temporal-change and
// onset guards added for general recordings. This is an original simplified
// implementation, not their complete algorithm, WPE, or a source separator.
// https://www.ee.columbia.edu/~dpwe/papers/LebBD01-ssdereverv.pdf
// https://israelcohen.com/wp-content/uploads/2018/05/IEEEI2008_Habets.pdf
//
// A nominal 0.6 s decay model estimates energy arriving after about 32 ms.
// Unlike a full RIR inverse, it reduces late energy and cannot undo early
// reflections or distinguish a wanted instrument decay from room decay.
// One real gain per bin preserves the original phases and stereo relation.
// All storage is fixed; this class performs no FFT, allocation, or locking.
class SpectralDereverb {
public:
    static constexpr std::size_t binCount = 1025;
    static constexpr std::size_t maximumDelayFrames = 32;

    SpectralDereverb() noexcept { prepare(48000.0, 2048, 512); }

    void prepare(double sampleRate, int fftSize = 2048, int hopSize = 512) noexcept {
        sampleRate_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
            ? sampleRate : 48000.0;
        const int validFftSize = std::max(2, fftSize);
        bins_ = std::clamp(validFftSize / 2 + 1, 1, static_cast<int>(binCount));
        const double hopSeconds = static_cast<double>(std::max(1, hopSize)) / sampleRate_;
        delayFrames_ = std::clamp(static_cast<int>(std::ceil(0.032 / hopSeconds)),
                                  1, static_cast<int>(maximumDelayFrames));
        // Energy falls by 60 dB in T60: exp(-ln(1e6)*t/T60).
        lateDecay_ = static_cast<float>(std::exp(-13.815510557964274 * hopSeconds * delayFrames_ / 0.6));
        powerStep_ = step(hopSeconds, 0.020);
        amountStep_ = step(hopSeconds, 0.025);
        openStep_ = step(hopSeconds, 0.008);
        closeStep_ = step(hopSeconds, 0.025);
        onsetHoldFrames_ = std::max(1, static_cast<int>(std::ceil(0.020 / hopSeconds)));
        reset();
    }

    void reset() noexcept {
        power_.fill(0.0f);
        smoothedPower_.fill(0.0f);
        priorPower_.fill(0.0f);
        targetGain_.fill(1.0f);
        gain_.fill(1.0f);
        onsetHold_.fill(0);
        historyCursor_ = validHistory_ = 0;
        currentAmount_ = targetAmount_;
        reductionDb_ = 0.0f;
        dormant_ = true;
    }

    void setAmount(float value) noexcept {
        if (std::isfinite(value)) targetAmount_ = std::clamp(value, 0.0f, 1.0f);
    }

    // Positive-frequency bins only. The caller owns conjugate reconstruction
    // and overlap-add. Bins beyond the prepared maximum are never accessed.
    void processFrame(std::complex<float>* left, std::complex<float>* right,
                      int bins, int channels) noexcept {
        if (left == nullptr || bins <= 0 || channels <= 0) { reductionDb_ = 0.0f; return; }
        const int count = std::min({bins, bins_, static_cast<int>(binCount)});
        const bool stereo = channels > 1 && right != nullptr;
        currentAmount_ += amountStep_ * (targetAmount_ - currentAmount_);
        if (std::abs(currentAmount_ - targetAmount_) < 1.0e-6f) currentAmount_ = targetAmount_;
        if (currentAmount_ == 0.0f) {
            if (!dormant_) reset();
            reductionDb_ = 0.0f;
            return;
        }
        dormant_ = false;

        for (int index = 0; index < count; ++index) {
            const auto k = static_cast<std::size_t>(index);
            left[k] = finiteComplex(left[k]);
            if (stereo) right[k] = finiteComplex(right[k]);
            const double l = power(left[k]);
            const double r = stereo ? power(right[k]) : l;
            power_[k] = static_cast<float>(std::min(1.0e30, 0.5 * (l + r)));
        }

        const float floorGain = std::exp(-1.72693881975f * currentAmount_); // Max 15 dB.
        const float floorPower = floorGain * floorGain;
        auto& delayed = history_[static_cast<std::size_t>(historyCursor_)];
        const bool historyReady = validHistory_ >= delayFrames_;
        for (int index = 0; index < count; ++index) {
            const auto k = static_cast<std::size_t>(index);
            const auto lo = static_cast<std::size_t>(std::max(0, index - 1));
            const auto hi = static_cast<std::size_t>(std::min(count - 1, index + 1));
            const float localPower = 0.25f * power_[lo] + 0.5f * power_[k] + 0.25f * power_[hi];
            const float previous = smoothedPower_[k];
            smoothedPower_[k] += powerStep_ * (localPower - smoothedPower_[k]);
            const float current = smoothedPower_[k];
            if (localPower > std::max(1.0e-18f, 1.75f * priorPower_[k])
                && current > previous * 1.15f)
                onsetHold_[k] = onsetHoldFrames_;
            else if (onsetHold_[k] > 0)
                --onsetHold_[k];
            priorPower_[k] = localPower;

            float candidate = 1.0f;
            if (historyReady && current > 1.0e-20f && onsetHold_[k] == 0) {
                const float older = delayed[k];
                // Steady harmonics remain open. Declining bins may carry late
                // energy from an earlier phoneme even while a new one is active.
                const float decayConfidence = std::clamp((older - current)
                    / (0.30f * older + 1.0e-20f), 0.0f, 1.0f);
                const float latePower = lateDecay_ * older;
                const float removal = 1.5f * currentAmount_ * decayConfidence
                    * latePower / (current + 1.0e-20f);
                candidate = std::sqrt(std::max(floorPower, 1.0f - std::min(1.0f, removal)));
            }
            targetGain_[k] = candidate;
            delayed[k] = current;
        }
        historyCursor_ = (historyCursor_ + 1) % delayFrames_;
        validHistory_ = std::min(validHistory_ + 1, delayFrames_);

        double before = 0.0, after = 0.0;
        for (int index = 0; index < count; ++index) {
            const auto k = static_cast<std::size_t>(index);
            const auto lo = static_cast<std::size_t>(std::max(0, index - 1));
            const auto hi = static_cast<std::size_t>(std::min(count - 1, index + 1));
            const float target = 0.25f * targetGain_[lo] + 0.5f * targetGain_[k] + 0.25f * targetGain_[hi];
            const float smoothing = target > gain_[k] ? openStep_ : closeStep_;
            gain_[k] += smoothing * (target - gain_[k]);
            if (std::abs(gain_[k] - target) < 1.0e-7f) gain_[k] = target;
            const float g = std::clamp(gain_[k], floorGain, 1.0f);
            left[k] *= g;
            if (stereo) right[k] *= g;
            before += power_[k];
            after += static_cast<double>(power_[k]) * g * g;
        }
        reductionDb_ = before > 1.0e-20
            ? static_cast<float>(std::clamp(-10.0 * std::log10(std::max(1.0e-12, after / before)), 0.0, 15.0)) : 0.0f;
    }

    float getReductionDb() const noexcept { return reductionDb_; }
    bool isActive() const noexcept { return targetAmount_ > 0.0f || currentAmount_ > 0.0f; }

private:
    static float step(double frameSeconds, double timeSeconds) noexcept {
        return static_cast<float>(1.0 - std::exp(-frameSeconds / timeSeconds));
    }
    static std::complex<float> finiteComplex(std::complex<float> value) noexcept {
        return {std::isfinite(value.real()) ? value.real() : 0.0f,
                std::isfinite(value.imag()) ? value.imag() : 0.0f};
    }
    static double power(std::complex<float> value) noexcept {
        const double real = value.real(), imag = value.imag();
        return real * real + imag * imag;
    }

    std::array<std::array<float, binCount>, maximumDelayFrames> history_ {};
    std::array<float, binCount> power_ {}, smoothedPower_ {}, priorPower_ {};
    std::array<float, binCount> targetGain_ {}, gain_ {};
    std::array<int, binCount> onsetHold_ {};
    double sampleRate_ = 48000.0;
    float lateDecay_ = 0.0f, powerStep_ = 0.0f, amountStep_ = 0.0f;
    float openStep_ = 0.0f, closeStep_ = 0.0f;
    float targetAmount_ = 0.0f, currentAmount_ = 0.0f, reductionDb_ = 0.0f;
    int bins_ = static_cast<int>(binCount), delayFrames_ = 3, historyCursor_ = 0;
    int validHistory_ = 0, onsetHoldFrames_ = 2;
    bool dormant_ = true;
};

} // namespace sculptclean

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <mutex>

extern "C" {
#include "../ThirdParty/RNNoise/include/rnnoise.h"
}

namespace sculptclean {

// Compact RNNoise v0.1 speech denoiser for the offline Studio render worker.
// Native rate is exactly 48 kHz. The file editor resamples before/after this
// adapter; unsupported rates fail prepare() instead of changing pitch/time.
// RNNoise uses a 480-sample hop and a 960-sample analysis window. Combined
// with streaming packet assembly, the dry/wet reference delay is 960 samples.
// This is speech noise suppression, not music/dialogue source separation.
class VoiceDenoiser {
public:
    static constexpr int frameSize = 480;
    static constexpr int latencySamples = 960;

    VoiceDenoiser() = default;
    ~VoiceDenoiser() { release(); }
    VoiceDenoiser(const VoiceDenoiser&) = delete;
    VoiceDenoiser& operator=(const VoiceDenoiser&) = delete;

    bool prepare(double sampleRate, int channels) {
        release();
        channels_ = std::clamp(channels, 1, 2);
        ready_ = std::isfinite(sampleRate) && std::abs(sampleRate - 48000.0) < 0.01;
        if (!ready_) { reset(); return false; }
        for (int ch = 0; ch < channels_; ++ch) {
            auto* storage = std::malloc(static_cast<std::size_t>(rnnoise_get_size()));
            if (storage == nullptr) { release(); reset(); return false; }
            states_[static_cast<std::size_t>(ch)] = static_cast<DenoiseState*>(storage);
            rnnoise_init(states_[static_cast<std::size_t>(ch)]);
        }
        // Upstream v0.1 lazily creates one shared FFT plan and its tables.
        // Serialize and warm that path in prepare, never in processSample.
        static std::once_flag commonPrepared;
        std::call_once(commonPrepared, [this] {
            std::array<float, frameSize> zero{}, output{};
            rnnoise_process_frame(states_[0], output.data(), zero.data());
        });
        reset();
        return true;
    }

    void reset() noexcept {
        for (auto* state : states_) if (state != nullptr) rnnoise_init(state);
        input_ = {}; output_ = {}; dry_ = {};
        position_ = dryPosition_ = 0;
        currentAmount_ = targetAmount_;
        probability_ = 0.0f;
        sleeping_ = currentAmount_ == 0.0f;
    }

    void setAmount(float value) noexcept {
        if (std::isfinite(value)) targetAmount_ = std::clamp(value, 0.0f, 1.0f);
    }
    bool isReady() const noexcept { return ready_; }
    static bool supportsSampleRate(double rate) noexcept {
        return std::isfinite(rate) && std::abs(rate - 48000.0) < 0.01;
    }
    int getLatencySamples() const noexcept { return latencySamples; }
    float getVoiceProbability() const noexcept { return probability_; }

    void processSample(float& left, float& right, int channels) noexcept {
        const int active = std::clamp(channels, 1, channels_);
        const float rawL = finite(left), rawR = active == 2 ? finite(right) : rawL;
        const float delayedL = dry_[0][dryPosition_], delayedR = dry_[1][dryPosition_];
        dry_[0][dryPosition_] = rawL; dry_[1][dryPosition_] = rawR;
        dryPosition_ = (dryPosition_ + 1) % latencySamples;
        currentAmount_ += amountStep_ * (targetAmount_ - currentAmount_);
        if (std::abs(currentAmount_ - targetAmount_) < 1.0e-7f) currentAmount_ = targetAmount_;
        const float mix = ready_ ? currentAmount_ : 0.0f;
        left = mix == 0.0f ? delayedL
            : finite(delayedL + mix * (output_[0][position_] - delayedL));
        right = active == 2 ? (mix == 0.0f ? delayedR
            : finite(delayedR + mix * (output_[1][position_] - delayedR))) : left;
        // Guard corrupt finite inputs without clipping the dry endpoint.
        input_[0][position_] = std::clamp(rawL, -4.0f, 4.0f) * 32768.0f;
        input_[1][position_] = std::clamp(rawR, -4.0f, 4.0f) * 32768.0f;
        if (++position_ != frameSize) return;
        position_ = 0;
        if (!ready_ || currentAmount_ == 0.0f) {
            if (!sleeping_) {
                for (auto* state : states_) if (state != nullptr) rnnoise_init(state);
                sleeping_ = true;
            }
            output_ = {}; probability_ = 0.0f;
            return;
        }
        sleeping_ = false;
        probability_ = 0.0f;
        for (int ch = 0; ch < active; ++ch) {
            const auto channel = static_cast<std::size_t>(ch);
            probability_ += rnnoise_process_frame(states_[channel], output_[channel].data(), input_[channel].data());
            for (float& value : output_[channel]) value = finite(value * (1.0f / 32768.0f));
        }
        probability_ = std::clamp(finite(probability_ / static_cast<float>(active)), 0.0f, 1.0f);
        if (active == 1) output_[1] = output_[0];
    }

private:
    static float finite(float value) noexcept { return std::isfinite(value) ? value : 0.0f; }
    void release() noexcept {
        for (auto*& state : states_) { if (state != nullptr) rnnoise_destroy(state); state = nullptr; }
        ready_ = false;
    }
    std::array<DenoiseState*, 2> states_{};
    std::array<std::array<float, frameSize>, 2> input_{}, output_{};
    std::array<std::array<float, latencySamples>, 2> dry_{};
    std::size_t position_ = 0, dryPosition_ = 0;
    int channels_ = 2;
    float targetAmount_ = 0.0f, currentAmount_ = 0.0f, probability_ = 0.0f;
    static constexpr float amountStep_ = 0.001041124f; // 20 ms at 48 kHz.
    bool ready_ = false, sleeping_ = true;
};
} // namespace sculptclean

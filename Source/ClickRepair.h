#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace sculptclean {

// Local impulse repair. A seven-sample window contributes a fixed three-sample
// delay even at zero amount. It targets isolated one-to-two-sample spikes;
// it is not a general mouth-click, crackle or clipped-waveform reconstruction.
// The slope guard rejects ordinary steep waveform changes. Musical impulses
// indistinguishable from faults can still be repaired: default amount is zero.
class ClickRepair {
public:
    static constexpr int latencySamples = 3;
    void prepare(double rate) noexcept {
        const double validRate = std::isfinite(rate) && rate >= 8000.0 ? rate : 48000.0;
        step_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.020 * validRate)));
        reset();
    }
    void reset() noexcept { history_ = {}; position_ = 0; amount_ = target_; }
    void setAmount(float value) noexcept {
        if (std::isfinite(value)) target_ = std::clamp(value, 0.0f, 1.0f);
    }
    void processSample(float& left, float& right, int channels) noexcept {
        smooth();
        history_[0][position_] = finite(left);
        history_[1][position_] = finite(channels > 1 ? right : left);
        const auto oldest = (position_ + 1) % 7;
        left = repair(0, oldest);
        right = channels > 1 ? repair(1, oldest) : left;
        position_ = oldest;
    }
private:
    static float finite(float x) noexcept { return std::isfinite(x) ? x : 0.0f; }
    void smooth() noexcept {
        amount_ += step_ * (target_ - amount_);
        if (std::abs(amount_ - target_) < 1.0e-7f) amount_ = target_;
    }
    float repair(std::size_t channel, std::size_t oldest) const noexcept {
        if (amount_ == 0.0f) return history_[channel][(oldest + 3) % 7];
        std::array<float, 7> x{};
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = history_[channel][(oldest + i) % 7];
        const double s0 = std::abs(static_cast<double>(x[1]) - x[0]);
        const double s1 = std::abs(static_cast<double>(x[2]) - x[1]);
        const double s2 = std::abs(static_cast<double>(x[5]) - x[4]);
        const double s3 = std::abs(static_cast<double>(x[6]) - x[5]);
        // The two middle slopes, without a per-sample four-element sort.
        const double middleA = std::max(std::min(s0, s1), std::min(s2, s3));
        const double middleB = std::min(std::max(s0, s1), std::max(s2, s3));
        const double localChange = 0.5 * (middleA + middleB);
        const double threshold = (18.0 - 14.0 * amount_) * localChange + 0.0015;
        // The median is outside the centre's tolerance band only when four
        // other samples are outside on the same side. Most audio needs no
        // seven-element sort at all; the decision is identical to full sorting.
        int above = 0, below = 0, inside = 0;
        for (const std::size_t index : {2U, 4U, 1U, 5U, 0U, 6U}) {
            const double difference = static_cast<double>(x[index]) - x[3];
            above += difference > threshold ? 1 : 0;
            below += difference < -threshold ? 1 : 0;
            inside += std::abs(difference) <= threshold ? 1 : 0;
            if (inside >= 3) return x[3];
        }
        if (above < 4 && below < 4) return x[3];
        auto ordered = x;
        std::sort(ordered.begin(), ordered.end());
        const double median = ordered[3];
        double replacement = median;
        if (std::abs(static_cast<double>(x[2]) - median) < threshold
            && std::abs(static_cast<double>(x[4]) - median) < threshold)
            replacement = 0.5 * (static_cast<double>(x[2]) + x[4]);
        return static_cast<float>(static_cast<double>(x[3])
            + amount_ * (replacement - x[3]));
    }
    std::array<std::array<float, 7>, 2> history_{};
    std::size_t position_ = 0;
    float amount_ = 0.0f, target_ = 0.0f, step_ = 0.001f;
};
} // namespace sculptclean

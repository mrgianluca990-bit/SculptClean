#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace sculptclean {

// Eight static mains-frequency notches, selectable 50/60 Hz; no extra FFT.
// RBJ notch formula: https://www.w3.org/TR/audio-eq-cookbook/
// Both coefficient sets are prepared ahead of audio processing. Changing
// mains frequency crossfades the banks; amount zero becomes exact dry.
class HumRemover {
public:
    void prepare(double rate) noexcept {
        rate_ = std::isfinite(rate) && rate >= 8000.0 && rate <= 384000.0 ? rate : 48000.0;
        step_ = 1.0 - std::exp(-1.0 / (0.020 * rate_));
        for (std::size_t bank = 0; bank < 2; ++bank)
            for (std::size_t harmonic = 0; harmonic < 8; ++harmonic) {
                const double frequency = (bank == 0 ? 50.0 : 60.0) * static_cast<double>(harmonic + 1);
                const double omega = 6.2831853071795864769 * frequency / rate_;
                const double alpha = std::sin(omega) / (2.0 * 50.0);
                const double scale = 1.0 / (1.0 + alpha);
                coefficients_[bank][harmonic] = { scale, -2.0 * std::cos(omega) * scale,
                    scale, -2.0 * std::cos(omega) * scale, (1.0 - alpha) * scale };
            }
        reset();
    }
    void reset() noexcept { states_ = {}; mix_ = targetMix_; bankMix_ = targetBank_; asleep_ = mix_ == 0.0; }
    void setParameters(float amount, float mainsHz) noexcept {
        if (std::isfinite(amount)) {
            const float bounded = std::clamp(amount, 0.0f, 1.0f);
            if (bounded != amountTarget_) {
                amountTarget_ = bounded;
                targetMix_ = 1.0 - std::pow(10.0, -24.0 * bounded / 20.0);
            }
        }
        if (std::isfinite(mainsHz)) {
            const double next = mainsHz >= 55.0f ? 1.0 : 0.0;
            if (next != targetBank_ && bankMix_ == 1.0 - next)
                states_[next > 0.0 ? 1 : 0] = {}; // Do not revive stale inactive history.
            targetBank_ = next;
        }
    }
    void processSample(float& left, float& right, int channels) noexcept {
        smooth(mix_, targetMix_); smooth(bankMix_, targetBank_);
        if (mix_ == 0.0) {
            if (!asleep_) { states_ = {}; asleep_ = true; }
            return;
        }
        asleep_ = false;
        const float rawL = left, rawR = channels > 1 ? right : left;
        double aL = rawL, aR = rawR, bL = rawL, bR = rawR;
        if (bankMix_ < 1.0) { aL = processBank(0, 0, rawL); if (channels > 1) aR = processBank(0, 1, rawR); }
        if (bankMix_ > 0.0) { bL = processBank(1, 0, rawL); if (channels > 1) bR = processBank(1, 1, rawR); }
        left = static_cast<float>(aL + bankMix_ * (bL - aL));
        right = channels > 1 ? static_cast<float>(aR + bankMix_ * (bR - aR)) : left;
    }
private:
    struct Coefficients { double b0=1, b1=0, b2=0, a1=0, a2=0; };
    struct State { double z1=0, z2=0; };
    void smooth(double& value, double target) const noexcept {
        value += step_ * (target - value);
        if (std::abs(value - target) < 1.0e-9) value = target;
    }
    double processBank(std::size_t bank, std::size_t channel, double input) noexcept {
        for (std::size_t h = 0; h < 8; ++h) {
            const auto& c = coefficients_[bank][h]; auto& s = states_[bank][channel][h];
            const double filtered = c.b0 * input + s.z1;
            s.z1 = c.b1 * input - c.a1 * filtered + s.z2;
            s.z2 = c.b2 * input - c.a2 * filtered;
            if (std::abs(s.z1) < 1.0e-30) s.z1 = 0.0;
            if (std::abs(s.z2) < 1.0e-30) s.z2 = 0.0;
            input += mix_ * (filtered - input);
        }
        return input;
    }
    std::array<std::array<Coefficients, 8>, 2> coefficients_{};
    std::array<std::array<std::array<State, 8>, 2>, 2> states_{};
    double rate_=48000, step_=0.001, mix_=0, targetMix_=0, bankMix_=0, targetBank_=0;
    float amountTarget_=0;
    bool asleep_=true;
};
} // namespace sculptclean

#include "../Source/SpectralDereverb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr int fftSize = 2048, hopSize = 512, bins = fftSize / 2 + 1;
constexpr double pi = 3.14159265358979323846;
using Spectrum = std::array<std::complex<float>, bins>;
int failures = 0;

void check(bool result, const char* message) {
    if (!result) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

double decibels(double numerator, double denominator) {
    return 10.0 * std::log10(std::max(1.0e-30, numerator) / std::max(1.0e-30, denominator));
}

std::uint32_t randomState = 0x7b3bda21u;
float noise() {
    randomState ^= randomState << 13u;
    randomState ^= randomState >> 17u;
    randomState ^= randomState << 5u;
    return static_cast<float>((static_cast<double>(randomState) / 4294967295.0) * 2.0 - 1.0);
}

void testSpectra() {
    sculptclean::SpectralDereverb effect;
    Spectrum left {}, right {};
    for (int k = 0; k < bins; ++k) {
        left[static_cast<std::size_t>(k)] = {noise(), noise()};
        right[static_cast<std::size_t>(k)] = {noise(), noise()};
    }
    const auto originalLeft = left, originalRight = right;
    for (int frame = 0; frame < 100; ++frame) effect.processFrame(left.data(), right.data(), bins, 2);
    check(left == originalLeft && right == originalRight, "amount zero must be bit-exact transparent");
    check(effect.getReductionDb() == 0.0f, "off reduction meter is zero");

    effect.setAmount(1.0f);
    effect.reset();
    for (int frame = 0; frame < 100; ++frame) {
        left = originalLeft;
        right = originalRight;
        effect.processFrame(left.data(), right.data(), bins, 2);
    }
    check(left == originalLeft && right == originalRight, "constant spectra must not lose wanted harmonics");

    double directBefore = 0.0, directAfter = 0.0, tailBefore = 0.0, tailAfter = 0.0;
    effect.reset();
    for (int frame = 0; frame < 100; ++frame) {
        const float amplitude = frame < 15 ? 1.0f
            : static_cast<float>(std::exp(-6.907755278982137 * (frame - 15) * hopSize / 48000.0 / 0.6));
        for (int k = 0; k < bins; ++k) {
            left[static_cast<std::size_t>(k)] = amplitude * originalLeft[static_cast<std::size_t>(k)];
            right[static_cast<std::size_t>(k)] = -left[static_cast<std::size_t>(k)];
        }
        const auto raw = left;
        effect.processFrame(left.data(), right.data(), bins, 2);
        for (int k = 0; k < bins; ++k) {
            const auto i = static_cast<std::size_t>(k);
            check(std::abs(right[i] + left[i]) < 1.0e-7f, "one gain must preserve stereo phase");
            if (frame < 15) { directBefore += std::norm(raw[i]); directAfter += std::norm(left[i]); }
            if (frame >= 20 && frame < 60) { tailBefore += std::norm(raw[i]); tailAfter += std::norm(left[i]); }
            check(std::abs(left[i]) <= std::abs(raw[i]) + 1.0e-6f, "dereverb must not boost bins");
            check(std::abs(left[i]) + 1.0e-6f >= 0.17782f * std::abs(raw[i]), "suppression is bounded to 15 dB");
        }
        check(std::isfinite(effect.getReductionDb()), "reduction meter remains finite");
    }
    const double directDb = decibels(directAfter, directBefore);
    const double tailDb = decibels(tailAfter, tailBefore);
    std::cout << "spectral_fixture_direct_change_db=" << directDb
              << " tail_change_db=" << tailDb << '\n';
    check(std::abs(directDb) < 0.001, "synthetic direct plateau must be unchanged");
    check(tailDb < -6.0, "known exponential spectral tail must be meaningfully reduced");

    effect.setAmount(0.0f);
    for (int frame = 0; frame < 150; ++frame) {
        left = originalLeft;
        right = originalRight;
        effect.processFrame(left.data(), right.data(), bins, 2);
    }
    check(left == originalLeft && right == originalRight, "automation to zero reaches exact transparency");

    for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        effect.prepare(rate, fftSize, hopSize);
        effect.setAmount(1.0f);
        effect.reset();
        left.fill({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()});
        effect.processFrame(left.data(), nullptr, bins, 1);
        for (const auto value : left)
            check(std::isfinite(value.real()) && std::isfinite(value.imag()), "active invalid input is sanitized");
        for (int frame = 0; frame < 200; ++frame) {
            left.fill({0.0f, 0.0f});
            effect.processFrame(left.data(), nullptr, bins, 1);
        }
        check(effect.getReductionDb() == 0.0f, "digital silence stays silent across sample rates");
    }
}

// A separate, simple test FFT/overlap-add renderer verifies actual audio. It is
// not the production shared-STFT implementation and does not hide its latency.
void fft(std::array<std::complex<float>, fftSize>& data, bool inverse) {
    for (int i = 1, j = 0; i < fftSize; ++i) {
        int bit = fftSize >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[static_cast<std::size_t>(i)], data[static_cast<std::size_t>(j)]);
    }
    for (int length = 2; length <= fftSize; length <<= 1) {
        const float angle = static_cast<float>((inverse ? 2.0 : -2.0) * pi / length);
        const std::complex<float> step {std::cos(angle), std::sin(angle)};
        for (int start = 0; start < fftSize; start += length) {
            std::complex<float> w {1.0f, 0.0f};
            for (int k = 0; k < length / 2; ++k) {
                const auto a = static_cast<std::size_t>(start + k);
                const auto b = static_cast<std::size_t>(start + k + length / 2);
                const auto u = data[a], v = data[b] * w;
                data[a] = u + v;
                data[b] = u - v;
                w *= step;
            }
        }
    }
    if (inverse) for (auto& value : data) value /= static_cast<float>(fftSize);
}

std::vector<float> render(const std::vector<float>& input, float amount) {
    std::vector<float> output(input.size(), 0.0f);
    std::array<float, fftSize> window {};
    for (int n = 0; n < fftSize; ++n)
        window[static_cast<std::size_t>(n)] = static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * pi * n / fftSize)));
    sculptclean::SpectralDereverb effect;
    effect.setAmount(amount);
    effect.reset();
    std::array<std::complex<float>, fftSize> frame {};
    for (int start = -fftSize + hopSize; start < static_cast<int>(input.size()); start += hopSize) {
        for (int n = 0; n < fftSize; ++n) {
            const int sample = start + n;
            frame[static_cast<std::size_t>(n)] = sample >= 0 && sample < static_cast<int>(input.size())
                ? input[static_cast<std::size_t>(sample)] * window[static_cast<std::size_t>(n)] : 0.0f;
        }
        fft(frame, false);
        effect.processFrame(frame.data(), nullptr, bins, 1);
        for (int k = 1; k < fftSize / 2; ++k) frame[static_cast<std::size_t>(fftSize - k)] = std::conj(frame[static_cast<std::size_t>(k)]);
        fft(frame, true);
        for (int n = 0; n < fftSize; ++n) {
            const int sample = start + n;
            if (sample >= 0 && sample < static_cast<int>(input.size()))
                output[static_cast<std::size_t>(sample)] += 0.5f * frame[static_cast<std::size_t>(n)].real() * window[static_cast<std::size_t>(n)];
        }
    }
    return output;
}

void testKnownRoomAudio() {
    constexpr int rate = 48000, length = rate * 6;
    const std::array<double, 6> starts {0.25, 1.15, 2.05, 2.95, 3.85, 4.75};
    const std::array<double, 6> durations {0.18, 0.24, 0.21, 0.19, 0.28, 0.22};
    std::vector<float> dry(length, 0.0f), wet(length, 0.0f);
    for (std::size_t burst = 0; burst < starts.size(); ++burst) {
        const int first = static_cast<int>(starts[burst] * rate);
        const int count = static_cast<int>(durations[burst] * rate);
        for (int n = 0; n < count; ++n) {
            const double t = static_cast<double>(n) / rate;
            const double envelope = std::min({1.0, t / 0.005, (durations[burst] - t) / 0.010});
            const double fundamental = 137.0 + 13.0 * static_cast<double>(burst);
            double value = 0.0;
            for (int harmonic = 1; harmonic <= 12; ++harmonic) {
                const double hz = fundamental * harmonic;
                const double formant = 0.25 + std::exp(-std::pow((hz - 650.0) / 350.0, 2.0))
                    + 0.6 * std::exp(-std::pow((hz - 1600.0) / 500.0, 2.0));
                value += formant * std::sin(2.0 * pi * hz * t + 0.14 * harmonic) / harmonic;
            }
            dry[static_cast<std::size_t>(first + n)] = static_cast<float>(envelope * (0.22 * value + 0.04 * noise()));
        }
    }
    // Sparse deterministic room: direct path, two early reflections and a
    // diffuse signed late tail. The tail has known 0.6 s amplitude T60 and
    // energy equal to 0.9 times the direct path, before early reflections.
    std::vector<std::pair<int, float>> room {{0, 1.0f}, {384, 0.18f}, {672, -0.13f}};
    double lateEnergy = 0.0;
    for (int tap = 0; tap < 192; ++tap) {
        const double t = 0.025 + 0.625 * (tap + 0.5 + 0.45 * noise()) / 192.0;
        const float coefficient = noise() * static_cast<float>(std::exp(-6.907755278982137 * t / 0.6));
        room.emplace_back(static_cast<int>(rate * t), coefficient);
        lateEnergy += coefficient * coefficient;
    }
    const float normalise = static_cast<float>(std::sqrt(0.9 / lateEnergy));
    for (std::size_t tap = 3; tap < room.size(); ++tap) room[tap].second *= normalise;
    for (const auto& tap : room)
        for (int n = tap.first; n < length; ++n)
            wet[static_cast<std::size_t>(n)] += tap.second * dry[static_cast<std::size_t>(n - tap.first)];

    const auto bypass = render(wet, 0.0f);
    const auto output = render(wet, 0.75f);
    double bypassError = 0.0, wetEnergy = 0.0;
    double tailInput = 0.0, tailOutput = 0.0, onsetInput = 0.0, onsetOutput = 0.0;
    double inputError = 0.0, outputError = 0.0;
    for (int n = 0; n < length; ++n) {
        const auto k = static_cast<std::size_t>(n);
        bypassError += std::pow(bypass[k] - wet[k], 2.0);
        wetEnergy += wet[k] * wet[k];
        inputError += std::pow(wet[k] - dry[k], 2.0);
        outputError += std::pow(output[k] - dry[k], 2.0);
        const double t = static_cast<double>(n) / rate;
        for (std::size_t burst = 0; burst < starts.size(); ++burst) {
            if (t >= starts[burst] && t < starts[burst] + 0.025) {
                onsetInput += wet[k] * wet[k];
                onsetOutput += output[k] * output[k];
            }
            if (t >= starts[burst] + durations[burst] + 0.055
                && t < starts[burst] + durations[burst] + 0.40) {
                tailInput += wet[k] * wet[k];
                tailOutput += output[k] * output[k];
            }
        }
    }
    const double bypassErrorDb = decibels(bypassError, wetEnergy);
    const double tailDb = decibels(tailOutput, tailInput);
    const double onsetDb = decibels(onsetOutput, onsetInput);
    const double errorChangeDb = decibels(outputError, inputError);
    std::cout << "known_room_amount=0.75 bypass_error_db=" << bypassErrorDb
              << " late_tail_change_db=" << tailDb << " onset_change_db=" << onsetDb
              << " error_vs_dry_change_db=" << errorChangeDb << '\n';
    check(bypassErrorDb < -90.0, "independent test STFT must reconstruct bypass accurately");
    check(tailDb < -3.0, "known convolved room late tail must decrease by at least 3 dB");
    check(onsetDb > -1.5 && onsetDb < 0.25, "known convolved direct onset must remain protected");
    check(errorChangeDb < 0.0, "known convolved room error to dry must improve");

    std::vector<float> steadyTone(rate, 0.0f);
    for (int n = 0; n < rate; ++n) {
        const double phase = 2.0 * pi * 440.0 * n / rate;
        steadyTone[static_cast<std::size_t>(n)] = static_cast<float>(0.3 * std::sin(phase)
            + 0.1 * std::sin(2.0 * phase) + 0.08 * std::sin(4.0 * phase));
    }
    const auto toneOutput = render(steadyTone, 1.0f);
    double toneInput = 0.0, toneEnergy = 0.0, toneError = 0.0;
    for (int n = rate / 4; n < 3 * rate / 4; ++n) {
        const auto k = static_cast<std::size_t>(n);
        toneInput += steadyTone[k] * steadyTone[k];
        toneEnergy += toneOutput[k] * toneOutput[k];
        toneError += std::pow(toneOutput[k] - steadyTone[k], 2.0);
    }
    const double toneChangeDb = decibels(toneEnergy, toneInput);
    const double toneErrorDb = decibels(toneError, toneInput);
    std::cout << "steady_dry_harmonics_level_change_db=" << toneChangeDb
              << " waveform_error_db=" << toneErrorDb << '\n';
    check(std::abs(toneChangeDb) < 0.1, "actual sustained dry harmonics retain their level");
    check(toneErrorDb < -40.0, "actual sustained dry harmonics retain their waveform");

    // Explicitly measure the fundamental limitation, without pretending to
    // classify a wanted instrumental fade as room energy: a dry exponential
    // note resembles the statistical late-room model and can be shortened.
    std::vector<float> dryNote(rate, 0.0f);
    for (int n = 0; n < rate; ++n) {
        const double t = static_cast<double>(n) / rate;
        dryNote[static_cast<std::size_t>(n)] = static_cast<float>(0.3 * std::min(1.0, t / 0.005)
            * std::exp(-6.907755278982137 * t / 0.8) * std::sin(2.0 * pi * 440.0 * t));
    }
    const auto noteOutput = render(dryNote, 0.75f);
    double noteIn = 0.0, noteOut = 0.0;
    for (int n = rate / 10; n < rate / 2; ++n) {
        noteIn += dryNote[static_cast<std::size_t>(n)] * dryNote[static_cast<std::size_t>(n)];
        noteOut += noteOutput[static_cast<std::size_t>(n)] * noteOutput[static_cast<std::size_t>(n)];
    }
    std::cout << "known_limitation_dry_decaying_note_tail_change_db=" << decibels(noteOut, noteIn) << '\n';
}
} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(4);
    testSpectra();
    testKnownRoomAudio();
    if (failures != 0) { std::cerr << failures << " dereverb checks failed\n"; return EXIT_FAILURE; }
    std::cout << "dereverb checks passed\n";
    return EXIT_SUCCESS;
}

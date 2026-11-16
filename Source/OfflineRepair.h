#pragma once

// Offline A-SPADE declipping / sparse inpainting.
// The alternating hard-threshold / consistency-projection / dual update is
// adapted from Andry Rafaralahy's MIT-licensed spade-declipping implementation:
// https://github.com/andryr/spade-declipping/tree/d9285bc5d6874670c462773f4c0dba55c6d7f66b
// Copyright (c) 2023 Andry Rafaralahy. License and unmodified upstream files:
// ThirdParty/Repair/spade-declipping/. Sculpt's C++ port, detector, cancellation,
// safeguards and missing-region projection are described in that folder's NOTICE.
// This code is deliberately OFFLINE: iterative FFTs and memory allocations
// belong on a worker thread, never in an AudioProcessor callback.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace sculptclean {

class OfflineRepair {
public:
    struct DeclipOptions {
        float threshold = 0.0f; // Absolute linear rail; zero detects positive/negative rails per channel.
        float tolerance = 0.001f; // Near-rail search band; a separate flatness check avoids ordinary peaks.
        float amount = 1.0f;
        float maxBoostDb = 12.0f;
        int iterations = 60;
        int fftSize = 2048;
        int minimumRun = 3;
    };
    struct RepairOptions {
        float amount = 1.0f;
        int iterations = 80;
        int fftSize = 2048;
        std::size_t maximumLength = 4096;
    };
    struct Report {
        bool ok = true, cancelled = false, changed = false;
        std::size_t regions = 0, samples = 0;
        float peakBefore = 0.0f, peakAfter = 0.0f;
        std::string message;
    };

    // Transactional: input buffers are written only after every channel has
    // completed. A cancellation or invalid request leaves every input intact.
    static Report declip(float* const* audio, int channels, std::size_t count,
                         double sampleRate, const DeclipOptions& options,
                         const std::atomic<bool>* cancel = nullptr,
                         std::atomic<float>* progress = nullptr) {
        Report report;
        setProgress(progress, 0.0f);
        if (stopped(cancel)) return cancelled(report);
        if (!validate(audio, channels, count, sampleRate, report, cancel)
            || !std::isfinite(options.threshold) || options.threshold < 0.0f
            || !std::isfinite(options.tolerance) || !std::isfinite(options.amount)
            || !std::isfinite(options.maxBoostDb)) return stopped(cancel) ? cancelled(report) : invalid(report);
        if (stopped(cancel)) return cancelled(report);
        const double amount = std::clamp(static_cast<double>(options.amount), 0.0, 1.0);
        if (amount == 0.0) return unchanged(report, progress);
        const double tolerance = std::clamp(static_cast<double>(options.tolerance), 0.0, 0.05);
        const auto nfft = fftLength(static_cast<std::size_t>(std::clamp(options.fftSize, 256, 16384)));
        const auto hop = nfft / 4;
        const auto frames = (count + hop - 1) / hop;
        const double boost = std::pow(10.0, std::clamp(static_cast<double>(options.maxBoostDb), 0.0, 24.0) / 20.0);
        const int iterations = std::clamp(options.iterations, 4, 200);
        const auto minimumRun = static_cast<std::size_t>(std::clamp(options.minimumRun, 2, 64));
        std::vector<std::vector<float>> output;
        output.reserve(static_cast<std::size_t>(channels));
        Solver solver(nfft);
        std::vector<double> window(nfft), reference(nfft), caps(nfft), accumulation(count), weight(count);
        std::vector<std::int8_t> mask(count), frameMask(nfft);
        makeWindow(window);
        for (int channel = 0; channel < channels; ++channel) {
            if (stopped(cancel)) return cancelled(report);
            const auto* source = audio[channel];
            output.emplace_back(source, source + count);
            std::fill(mask.begin(), mask.end(), 0);
            std::fill(accumulation.begin(), accumulation.end(), 0.0);
            std::fill(weight.begin(), weight.end(), 0.0);
            double positiveRail = 0.0, negativeRail = 0.0;
            for (std::size_t i = 0; i < count; ++i) {
                positiveRail = std::max(positiveRail, static_cast<double>(source[i]));
                negativeRail = std::max(negativeRail, -static_cast<double>(source[i]));
            }
            const double observedPeak = std::max(positiveRail, negativeRail);
            if (options.threshold > 0.0f) positiveRail = negativeRail = options.threshold;
            const auto marked = markClipping(source, count, positiveRail, negativeRail, tolerance, minimumRun, mask);
            report.regions += marked;
            if (marked == 0) {
                setProgress(progress, static_cast<float>(channel + 1) / static_cast<float>(channels));
                continue;
            }
            const double maximumPeak = std::max({positiveRail, negativeRail, observedPeak}) * boost;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                if (stopped(cancel)) return cancelled(report);
                const auto start = frame * hop;
                const auto available = std::min(nfft, count - start);
                bool needsRepair = false;
                for (std::size_t i = 0; i < nfft; ++i) {
                    const bool valid = i < available;
                    frameMask[i] = valid ? mask[start + i] : 0;
                    reference[i] = valid ? static_cast<double>(source[start + i]) * window[i] : 0.0;
                    caps[i] = maximumPeak * window[i];
                    needsRepair = needsRepair || frameMask[i] != 0;
                }
                if (needsRepair) {
                    if (!solver.solve(reference, frameMask, caps, iterations, cancel)) return cancelled(report);
                    for (std::size_t i = 0; i < available; ++i) {
                        if (frameMask[i] == 0) continue;
                        accumulation[start + i] += solver.result()[i] * window[i];
                        weight[start + i] += window[i] * window[i];
                    }
                }
                setProgress(progress, static_cast<float>((static_cast<double>(channel)
                    + static_cast<double>(frame + 1) / static_cast<double>(frames)) / channels));
            }
            auto& destination = output.back();
            for (std::size_t i = 0; i < count; ++i) {
                if (mask[i] == 0 || weight[i] <= 0.0) continue;
                double repaired = accumulation[i] / weight[i];
                repaired = mask[i] > 0 ? std::max(repaired, static_cast<double>(source[i]))
                                        : std::min(repaired, static_cast<double>(source[i]));
                repaired = std::clamp(repaired, -maximumPeak, maximumPeak);
                destination[i] = static_cast<float>(source[i] + amount * (repaired - source[i]));
            }
        }
        if (stopped(cancel)) return cancelled(report);
        commit(audio, output, count, report);
        setProgress(progress, 1.0f);
        report.message = report.changed ? "De-clip completato; controlla i picchi ricostruiti e il livello di uscita."
                                        : "Nessun tratto di clipping piatto riparabile rilevato.";
        return report;
    }

    static Report repairRegion(float* const* audio, int channels, std::size_t count,
                               double sampleRate, std::size_t start, std::size_t length,
                               const RepairOptions& options,
                               const std::atomic<bool>* cancel = nullptr,
                               std::atomic<float>* progress = nullptr) {
        Report report;
        setProgress(progress, 0.0f);
        if (stopped(cancel)) return cancelled(report);
        if (!validate(audio, channels, count, sampleRate, report, cancel)
            || !std::isfinite(options.amount) || start > count || length > count - start)
            return stopped(cancel) ? cancelled(report) : invalid(report);
        if (stopped(cancel)) return cancelled(report);
        if (length == 0 || options.amount <= 0.0f) return unchanged(report, progress);
        if (length > std::min<std::size_t>(4096, options.maximumLength)) {
            report.ok = false;
            report.message = "Selezione troppo lunga: Repair ricostruisce al massimo 4096 campioni per intervento.";
            return report;
        }
        if (start < 16 || count - start - length < 16) {
            report.ok = false;
            report.message = "Repair richiede almeno 16 campioni integri su entrambi i lati della selezione.";
            return report;
        }
        const double amount = std::clamp(static_cast<double>(options.amount), 0.0, 1.0);
        const auto requested = std::max<std::size_t>(static_cast<std::size_t>(std::clamp(options.fftSize, 256, 16384)), length * 4);
        const auto nfft = fftLength(requested);
        const std::int64_t centre = static_cast<std::int64_t>(start + length / 2);
        const std::int64_t origin = centre - static_cast<std::int64_t>(nfft / 2);
        const auto gapOffset = static_cast<std::size_t>(static_cast<std::int64_t>(start) - origin);
        const int iterations = std::clamp(options.iterations, 4, 200);
        Solver solver(nfft);
        std::vector<double> window(nfft), reference(nfft), caps(nfft);
        std::vector<std::int8_t> mask(nfft);
        std::vector<std::vector<float>> replacement(static_cast<std::size_t>(channels), std::vector<float>(length));
        makeWindow(window);
        for (int channel = 0; channel < channels; ++channel) {
            if (stopped(cancel)) return cancelled(report);
            const auto* source = audio[channel];
            double reliablePeak = 1.0e-5;
            for (std::size_t i = 0; i < nfft; ++i) {
                const auto position = origin + static_cast<std::int64_t>(i);
                const bool inFile = position >= 0 && position < static_cast<std::int64_t>(count);
                const bool inGap = i >= gapOffset && i < gapOffset + length;
                mask[i] = inGap ? 2 : 0;
                double value = inFile ? source[static_cast<std::size_t>(position)] : 0.0;
                if (inGap) {
                    const double t = static_cast<double>(i - gapOffset + 1) / static_cast<double>(length + 1);
                    value = (1.0 - t) * source[start - 1] + t * source[start + length];
                } else reliablePeak = std::max(reliablePeak, std::abs(value));
                reference[i] = value * window[i];
            }
            for (std::size_t i = 0; i < nfft; ++i) caps[i] = reliablePeak * 4.0 * window[i];
            if (!solver.solve(reference, mask, caps, iterations, cancel)) return cancelled(report);
            auto& destination = replacement[static_cast<std::size_t>(channel)];
            for (std::size_t i = 0; i < length; ++i) {
                const auto index = gapOffset + i;
                const double value = solver.result()[index] / window[index];
                destination[i] = static_cast<float>(source[start + i] + amount * (value - source[start + i]));
            }
            setProgress(progress, static_cast<float>(channel + 1) / static_cast<float>(channels));
        }
        if (stopped(cancel)) return cancelled(report);
        for (int channel = 0; channel < channels; ++channel) {
            for (std::size_t i = 0; i < length; ++i) {
                const float value = replacement[static_cast<std::size_t>(channel)][i];
                if (value != audio[channel][start + i]) ++report.samples;
                audio[channel][start + i] = value;
            }
        }
        report.regions = static_cast<std::size_t>(channels);
        report.changed = report.samples != 0;
        report.peakAfter = peak(audio, channels, count);
        report.message = "Repair completato; confronta la selezione con l'originale.";
        setProgress(progress, 1.0f);
        return report;
    }

private:
    static constexpr double pi = 3.1415926535897932384626433832795;
    static bool stopped(const std::atomic<bool>* flag) noexcept { return flag != nullptr && flag->load(std::memory_order_relaxed); }
    static void setProgress(std::atomic<float>* progress, float value) noexcept { if (progress != nullptr) progress->store(value, std::memory_order_relaxed); }
    static Report invalid(Report report) { report.ok = false; report.message = "Audio o parametri non validi: nessuna modifica applicata."; return report; }
    static Report cancelled(Report report) { report.ok = false; report.cancelled = true; report.changed = false; report.samples = report.regions = 0; report.peakAfter = report.peakBefore; report.message = "Operazione annullata; audio originale conservato."; return report; }
    static Report unchanged(Report report, std::atomic<float>* progress) { report.peakAfter = report.peakBefore; report.message = "Nessuna modifica richiesta."; setProgress(progress, 1.0f); return report; }
    static float peak(float* const* audio, int channels, std::size_t count) {
        float result = 0.0f;
        for (int c = 0; c < channels; ++c) for (std::size_t i = 0; i < count; ++i) result = std::max(result, std::abs(audio[c][i]));
        return result;
    }
    static bool validate(float* const* audio, int channels, std::size_t count, double rate, Report& report,
                         const std::atomic<bool>* cancel) {
        if (audio == nullptr || channels < 1 || channels > 64 || count == 0 || !std::isfinite(rate) || rate < 8000.0 || rate > 384000.0
            || count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / 4)) return false;
        for (int c = 0; c < channels; ++c) {
            if (audio[c] == nullptr) return false;
            for (std::size_t i = 0; i < count; ++i) {
                if ((i & 4095U) == 0 && stopped(cancel)) return false;
                if (!std::isfinite(audio[c][i]) || std::abs(audio[c][i]) > 1.0e6f) return false;
                report.peakBefore = std::max(report.peakBefore, std::abs(audio[c][i]));
            }
        }
        report.peakAfter = report.peakBefore;
        return true;
    }
    static std::size_t fftLength(std::size_t desired) noexcept {
        std::size_t length = 256;
        while (length < desired && length < 16384) length *= 2;
        return length;
    }
    static void makeWindow(std::vector<double>& window) {
        for (std::size_t i = 0; i < window.size(); ++i)
            window[i] = std::sqrt(0.54 - 0.46 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(window.size() - 1)));
    }
    static std::size_t markClipping(const float* source, std::size_t count,
                                    double positiveRail, double negativeRail, double tolerance,
                                    std::size_t minimumRun, std::vector<std::int8_t>& mask) {
        std::size_t regions = 0;
        for (std::size_t start = 0; start < count;) {
            const int sign = source[start] >= 0.0f ? 1 : -1;
            const double rail = sign > 0 ? positiveRail : negativeRail;
            const double level = sign * static_cast<double>(source[start]);
            if (rail < 1.0e-7 || level < rail * (1.0 - tolerance)) { ++start; continue; }
            // Only near-flat digital rails, not a broad band around every
            // rounded musical peak. Lossy/soft/dithered clipping needs a more
            // sophisticated detector and is not claimed here.
            const double flatTolerance = std::max(1.0e-9, rail * 1.0e-6);
            std::size_t end = start + 1;
            while (end < count && sign * static_cast<double>(source[end]) >= rail * (1.0 - tolerance)
                && std::abs(static_cast<double>(source[end]) - source[start]) <= flatTolerance) ++end;
            if (end - start >= minimumRun && start > 0 && end < count) {
                for (std::size_t i = start; i < end; ++i) mask[i] = static_cast<std::int8_t>(sign);
                ++regions;
            }
            start = end;
        }
        return regions;
    }
    static void commit(float* const* audio, const std::vector<std::vector<float>>& output,
                       std::size_t count, Report& report) {
        report.peakAfter = 0.0f;
        for (std::size_t c = 0; c < output.size(); ++c) for (std::size_t i = 0; i < count; ++i) {
            const float value = output[c][i];
            if (value != audio[c][i]) ++report.samples;
            audio[c][i] = value;
            report.peakAfter = std::max(report.peakAfter, std::abs(value));
        }
        report.changed = report.samples != 0;
    }

    class Solver {
    public:
        explicit Solver(std::size_t size)
            : size_(size), scale_(1.0 / std::sqrt(static_cast<double>(size))),
              reverse_(size), twiddle_(size / 2), x_(size), ax_(size), dual_(size), sparse_(size), work_(size), power_(size), ranked_(size) {
            unsigned bits = 0;
            for (auto n = size; n > 1; n >>= 1U) ++bits;
            for (std::size_t i = 0; i < size; ++i) {
                auto value = i; std::size_t reverse = 0;
                for (unsigned bit = 0; bit < bits; ++bit) { reverse = (reverse << 1U) | (value & 1U); value >>= 1U; }
                reverse_[i] = reverse;
            }
            for (std::size_t i = 0; i < twiddle_.size(); ++i) {
                const double phase = -2.0 * pi * static_cast<double>(i) / static_cast<double>(size);
                twiddle_[i] = {std::cos(phase), std::sin(phase)};
            }
        }
        const std::vector<double>& result() const noexcept { return x_; }
        bool solve(const std::vector<double>& reference, const std::vector<std::int8_t>& mask,
                   const std::vector<double>& cap, int iterations, const std::atomic<bool>* cancel) {
            x_ = reference;
            std::fill(dual_.begin(), dual_.end(), std::complex<double>{});
            for (std::size_t i = 0; i < size_; ++i) ax_[i] = {x_[i], 0.0};
            transform(ax_, false);
            for (int iteration = 0; iteration < iterations; ++iteration) {
                if (stopped(cancel)) return false;
                // MIT tight_aspade.m: z = H_k(A*x + u).
                for (std::size_t i = 0; i < size_; ++i) { work_[i] = ax_[i] + dual_[i]; power_[i] = std::norm(work_[i]); }
                // Keep conjugate pairs together, including close floating
                // point ties, so the synthesis remains a real waveform.
                for (std::size_t i = 1; i < size_ / 2; ++i) power_[i] = power_[size_ - i] = 0.5 * (power_[i] + power_[size_ - i]);
                ranked_ = power_;
                const auto keep = std::min(size_, static_cast<std::size_t>(4 + 4 * (iteration / 4)));
                std::nth_element(ranked_.begin(), ranked_.begin() + static_cast<std::ptrdiff_t>(keep - 1), ranked_.end(), std::greater<double>());
                const double cutoff = ranked_[keep - 1];
                for (std::size_t i = 0; i < size_; ++i) { sparse_[i] = power_[i] >= cutoff ? work_[i] : std::complex<double>{}; work_[i] = sparse_[i] - dual_[i]; }
                transform(work_, true);
                // MIT consistency projection, plus a bounded free projection
                // for a manually selected missing region (mask == 2).
                for (std::size_t i = 0; i < size_; ++i) {
                    double value = work_[i].real();
                    if (mask[i] == 0) value = reference[i];
                    else if (mask[i] == 1) value = std::max(value, reference[i]);
                    else if (mask[i] == -1) value = std::min(value, reference[i]);
                    if (mask[i] != 0) value = std::clamp(value, -cap[i], cap[i]);
                    x_[i] = value;
                    ax_[i] = {value, 0.0};
                }
                transform(ax_, false);
                double residual = 0.0, energy = 0.0;
                for (std::size_t i = 0; i < size_; ++i) {
                    const auto error = ax_[i] - sparse_[i];
                    residual += std::norm(error); energy += std::norm(ax_[i]);
                    dual_[i] += error;
                }
                if (iteration >= 8 && residual <= 1.0e-10 * std::max(1.0e-20, energy)) break;
            }
            return true;
        }
    private:
        void transform(std::vector<std::complex<double>>& data, bool inverse) const {
            for (std::size_t i = 0; i < size_; ++i) if (reverse_[i] > i) std::swap(data[i], data[reverse_[i]]);
            for (std::size_t length = 2; length <= size_; length *= 2) {
                const auto half = length / 2, stride = size_ / length;
                for (std::size_t start = 0; start < size_; start += length) for (std::size_t k = 0; k < half; ++k) {
                    const auto w = inverse ? std::conj(twiddle_[k * stride]) : twiddle_[k * stride];
                    const auto even = data[start + k], odd = data[start + k + half] * w;
                    data[start + k] = even + odd; data[start + k + half] = even - odd;
                }
            }
            for (auto& value : data) value *= scale_;
        }
        std::size_t size_;
        double scale_;
        std::vector<std::size_t> reverse_;
        std::vector<std::complex<double>> twiddle_;
        std::vector<double> x_;
        std::vector<std::complex<double>> ax_, dual_, sparse_, work_;
        std::vector<double> power_, ranked_;
    };
};
} // namespace sculptclean

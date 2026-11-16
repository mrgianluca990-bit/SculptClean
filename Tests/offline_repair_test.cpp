#include "../Source/OfflineRepair.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
constexpr double pi = 3.14159265358979323846;
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
double error(const std::vector<float>& a, const std::vector<float>& b, std::size_t first = 0, std::size_t length = 0) {
    if (length == 0) length = a.size() - first;
    double result = 0;
    for (std::size_t i = first; i < first + length; ++i) { const double d = a[i] - b[i]; result += d * d; }
    return result;
}
std::vector<float> harmonic(std::size_t length, double frequency = 441.0) {
    std::vector<float> samples(length);
    for (std::size_t i = 0; i < length; ++i) {
        const double phase = 2.0 * pi * frequency * static_cast<double>(i) / 48000.0;
        samples[i] = static_cast<float>(.58 * std::sin(phase) + .17 * std::sin(2.0 * phase + .3) + .08 * std::cos(3.0 * phase + .2));
    }
    return samples;
}
void declipKnown() {
    auto original = harmonic(12000);
    auto clipped = original;
    for (auto& x : clipped) x = std::clamp(x, -.38f, .38f);
    auto result = clipped;
    float* channel[]{result.data()};
    sculptclean::OfflineRepair::DeclipOptions options;
    std::atomic<float> progress{0};
    const auto start = std::chrono::steady_clock::now();
    const auto report = sculptclean::OfflineRepair::declip(channel, 1, result.size(), 48000.0, options, nullptr, &progress);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double improvement = 10.0 * std::log10(error(original, clipped) / error(original, result));
    require(report.ok && report.changed && report.regions > 0 && report.samples > 0, "Clipping was not repaired");
    require(improvement > 2.0, "Sparse de-clip did not improve known harmonic fixture");
    require(progress == 1.0f, "De-clip progress did not finish");
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(std::isfinite(result[i]), "Non-finite de-clip output");
        if (std::abs(clipped[i]) < .38f) require(result[i] == clipped[i], "Reliable samples changed");
        else require(std::abs(result[i]) + 1.0e-7f >= std::abs(clipped[i]), "Clipping consistency violated");
    }
    std::cout << "declip_harmonic_error_improvement_db=" << improvement
              << " regions=" << report.regions << " restored_peak=" << report.peakAfter
              << " render_seconds_for_0.25s_mono=" << seconds << '\n';
}
void declipModulated() {
    std::vector<float> dry(10000);
    for (std::size_t n = 0; n < dry.size(); ++n) {
        const double time = static_cast<double>(n) / 48000.0;
        dry[n] = static_cast<float>((.7 + .3 * std::sin(2 * pi * 4 * time))
            * (.3 * std::sin(2 * pi * 233 * time) + .13 * std::sin(2 * pi * 709 * time + .1) + .08 * std::sin(2 * pi * 1360 * time)));
    }
    auto clipped = dry;
    for (auto& x : clipped) x = std::clamp(x, -.24f, .24f);
    auto output = clipped;
    float* channel[]{output.data()};
    sculptclean::OfflineRepair::DeclipOptions options;
    auto report = sculptclean::OfflineRepair::declip(channel, 1, output.size(), 48000, options);
    const double improvement = 10 * std::log10(error(dry, clipped) / error(dry, output));
    require(report.ok && report.changed && improvement > 1.0, "Modulated de-clip fixture did not improve");
    std::cout << "declip_modulated_error_improvement_db=" << improvement << '\n';
}
void repairKnown() {
    const auto original = harmonic(8000);
    for (const std::size_t length : {16U, 64U, 128U}) {
        constexpr std::size_t begin = 3500;
        auto damaged = original;
        for (std::size_t i = begin; i < begin + length; ++i) damaged[i] = i % 2 == 0 ? .9f : -.9f;
        auto output = damaged;
        float* channels[]{output.data()};
        sculptclean::OfflineRepair::RepairOptions options;
        const auto report = sculptclean::OfflineRepair::repairRegion(channels, 1, output.size(), 48000, begin, length, options);
        const double improvement = 10 * std::log10(error(original, damaged, begin, length) / error(original, output, begin, length));
        require(report.ok && report.changed && improvement > 10, "Manual repair did not improve known damaged region");
        for (std::size_t i = 0; i < output.size(); ++i) if (i < begin || i >= begin + length)
            require(output[i] == damaged[i], "Manual repair changed audio outside selection");
        std::cout << "repair_" << length << "_sample_gap_error_improvement_db=" << improvement << '\n';
    }
}
void identityAndValidation() {
    sculptclean::OfflineRepair::DeclipOptions declip;
    sculptclean::OfflineRepair::RepairOptions repair;
    for (const double frequency : {20.0,110.0,997.0,4001.0}) {
        auto audio = harmonic(5000, frequency); const auto original = audio;
        float* channels[]{audio.data()};
        auto report = sculptclean::OfflineRepair::declip(channels, 1, audio.size(), 48000, declip);
        require(report.ok && audio == original, "Unclipped tonal input changed");
    }
    auto audio = harmonic(5000); const auto original = audio;
    float* channels[]{audio.data()};
    declip.amount = 0;
    auto report = sculptclean::OfflineRepair::declip(channels, 1, audio.size(), 48000, declip);
    require(report.ok && audio == original, "Zero amount changed input");
    report = sculptclean::OfflineRepair::repairRegion(channels, 1, audio.size(), 48000, 0, 32, repair);
    require(!report.ok && audio == original, "Edge repair should reject insufficient context");
    report = sculptclean::OfflineRepair::repairRegion(channels, 1, audio.size(), 48000, 32, 4097, repair);
    require(!report.ok && audio == original, "Overlong repair was not rejected");
    report = sculptclean::OfflineRepair::repairRegion(channels, 1, audio.size(), 48000, 5001, 1, repair);
    require(!report.ok && audio == original, "Invalid selection changed input");
    std::atomic<bool> stop{true};
    report = sculptclean::OfflineRepair::repairRegion(channels, 1, audio.size(), 48000, 100, 32, repair, &stop);
    require(report.cancelled && audio == original, "Cancellation was not transactional");
    audio[125] = std::numeric_limits<float>::infinity();
    declip.amount = 1;
    report = sculptclean::OfflineRepair::declip(channels, 1, audio.size(), 48000, declip);
    require(!report.ok && std::isinf(audio[125]), "Invalid input was partly overwritten");
    audio = original;
    for (auto& sample : audio) sample = std::clamp(sample,-.38f,.38f);
    const auto clipped = audio;
    channels[0] = audio.data();
    declip.threshold = .2f;declip.maxBoostDb = 0.0f;
    report = sculptclean::OfflineRepair::declip(channels,1,audio.size(),48000,declip);
    require(report.ok && audio == clipped,"A low user threshold violated consistency by lowering clipped peaks");
    std::cout << "identity_unclipped_zero_amount_edge_limits_cancellation_validation=passed\n";
}
void stereo() {
    auto left = harmonic(6000), right = left;
    for (std::size_t i = 0; i < left.size(); ++i) { left[i] = std::clamp(left[i],-.38f,.38f); right[i] = -left[i]; }
    float* channels[]{left.data(),right.data()};
    sculptclean::OfflineRepair::DeclipOptions options;
    auto report = sculptclean::OfflineRepair::declip(channels, 2, left.size(), 48000, options);
    require(report.ok && report.changed, "Stereo de-clip failed");
    double maximum = 0;
    for (std::size_t i = 0; i < left.size(); ++i) maximum = std::max(maximum,std::abs(static_cast<double>(left[i])+right[i]));
    require(maximum < 1.0e-6, "Stereo anti-phase relation changed");
    std::cout << "stereo_antiphase_max_error=" << maximum << '\n';
}
void cancelDuringRenderAndTransient() {
    auto left = harmonic(20000), right = left;
    for (std::size_t i = 0; i < left.size(); ++i) {left[i] = std::clamp(left[i],-.38f,.38f);right[i] = -left[i];}
    const auto originalL = left, originalR = right;
    float* channels[]{left.data(),right.data()};
    std::atomic<float> progress{0};std::atomic<bool> stop{false},done{false};
    std::thread stopper([&]{while(progress.load(std::memory_order_relaxed)<.02f&&!done.load(std::memory_order_relaxed))std::this_thread::yield();stop.store(true,std::memory_order_relaxed);});
    sculptclean::OfflineRepair::DeclipOptions options;
    const auto report = sculptclean::OfflineRepair::declip(channels,2,left.size(),48000,options,&stop,&progress);
    done.store(true,std::memory_order_relaxed);stopper.join();
    require(report.cancelled && left==originalL && right==originalR,"Mid-render cancellation left a partial edit");
    std::vector<float> transient(5000,0.0f);transient[900]=.9f;transient[901]=-.4f;
    const auto original=transient;float* ptrs[]{transient.data()};
    const auto untouched=sculptclean::OfflineRepair::declip(ptrs,1,transient.size(),48000,options);
    require(untouched.ok && transient==original,"Wanted short transient was classified as clipping");
    std::cout<<"mid_render_transactional_cancel_and_short_transient_identity=passed\n";
}
}
int main() {
    try { declipKnown(); declipModulated(); repairKnown(); identityAndValidation(); stereo(); cancelDuringRenderAndTransient(); std::cout << "Offline repair tests passed.\n"; return 0; }
    catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

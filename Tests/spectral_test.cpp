#include "../Source/SpectralCleaner.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
std::atomic<bool> tracking{false};
std::atomic<std::size_t> allocationCount{0};
void countAllocation() noexcept {
    if (tracking.load(std::memory_order_relaxed)) allocationCount.fetch_add(1, std::memory_order_relaxed);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void releaseMemory(void* pointer) noexcept { std::free(pointer); }
}
void* operator new(std::size_t count) {
    countAllocation();
    if (void* pointer = std::malloc(count == 0 ? 1 : count)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t count) { return ::operator new(count); }
void operator delete(void* pointer) noexcept { releaseMemory(pointer); }
void operator delete[](void* pointer) noexcept { releaseMemory(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { releaseMemory(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { releaseMemory(pointer); }
void* operator new(std::size_t count, std::align_val_t alignment) {
    countAllocation();
    void* pointer = nullptr;
    if (posix_memalign(&pointer, std::max(sizeof(void*), static_cast<std::size_t>(alignment)),
                       count == 0 ? 1 : count) == 0) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t count, std::align_val_t alignment) { return ::operator new(count, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept { releaseMemory(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { releaseMemory(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { releaseMemory(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { releaseMemory(pointer); }

namespace {
using sculptclean::SpectralCleaner;
constexpr std::size_t latency = SpectralCleaner::fftSize;
constexpr double sampleRate = 48000.0;
constexpr double pi = 3.1415926535897932384626433832795;

struct Audio {
    std::vector<float> left, right;
    explicit Audio(std::size_t count) : left(count), right(count) {}
    std::size_t size() const { return left.size(); }
};

struct Random {
    std::uint32_t state = 0x12345678U;
    float noise() noexcept {
        state ^= state << 13U; state ^= state >> 17U; state ^= state << 5U;
        return static_cast<float>(state >> 8U) / 8388608.0f - 1.0f;
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Audio white(std::size_t count, std::uint32_t seed = 0x12345678U, float level = 0.03f) {
    Audio result(count); Random random{seed};
    for (std::size_t i = 0; i < count; ++i) {
        result.left[i] = level * random.noise();
        result.right[i] = level * random.noise();
    }
    return result;
}

// The sample API deliberately permits any host block size. This helper tests
// that changing the grouping of samples cannot reset a hop or FFT history.
void run(SpectralCleaner& engine, Audio& audio, std::size_t block = 127, int channels = 2) {
    for (std::size_t offset = 0; offset < audio.size(); offset += block) {
        const std::size_t end = std::min(audio.size(), offset + block);
        for (std::size_t i = offset; i < end; ++i) engine.processSample(audio.left[i], audio.right[i], channels);
    }
    for (std::size_t i = 0; i < audio.size(); ++i)
        require(std::isfinite(audio.left[i]) && std::isfinite(audio.right[i]), "Spectral output is nonfinite");
}

double db(double numerator, double denominator) {
    return 10.0 * std::log10(std::max(1.0e-30, numerator) / std::max(1.0e-30, denominator));
}

double attenuation(const Audio& output, const Audio& input, std::size_t begin, std::size_t end) {
    double before = 0.0, after = 0.0;
    require(end + latency <= output.size() && end <= input.size(), "Invalid delayed comparison range");
    for (std::size_t i = begin; i < end; ++i) {
        before += static_cast<double>(input.left[i]) * input.left[i] + static_cast<double>(input.right[i]) * input.right[i];
        after += static_cast<double>(output.left[i + latency]) * output.left[i + latency]
            + static_cast<double>(output.right[i + latency]) * output.right[i + latency];
    }
    return db(after, before);
}

SpectralCleaner::LearnedState learn(SpectralCleaner& engine, double rate = sampleRate, int channels = 2) {
    engine.requestNoiseLearn();
    auto audio = white(static_cast<std::size_t>(2.05 * rate));
    run(engine, audio, 257, channels);
    const auto meter = engine.getMeters();
    require(meter.noiseProfileReady && !meter.noiseLearning, "Explicit noise capture did not finish after two seconds");
    return engine.getLearnedState();
}

void testNeutralDelay() {
    for (double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        for (int channels : {1, 2}) {
            SpectralCleaner engine; engine.prepare(rate, channels);
            require(engine.getLatencySamples() == static_cast<int>(latency), "Spectral latency is not 2048 samples");
            auto output = white(latency * 4); const auto input = output;
            run(engine, output, 63, channels);
            for (std::size_t i = 0; i < output.size(); ++i) {
                const float expectedL = i < latency ? 0.0f : input.left[i - latency];
                const float expectedR = i < latency ? 0.0f : (channels == 1 ? input.left[i - latency] : input.right[i - latency]);
                require(output.left[i] == expectedL && output.right[i] == expectedR,
                        "Neutral STFT is not an exact delayed copy, including startup");
            }
            engine.reset();
            Audio impulse(latency * 2); impulse.left[0] = 0.5f; impulse.right[0] = -0.5f;
            run(engine, impulse, 1, channels);
            for (std::size_t i = 0; i < impulse.size(); ++i) {
                require(impulse.left[i] == (i == latency ? 0.5f : 0.0f), "Impulse latency or reconstruction is wrong");
                const float expected = i == latency ? (channels == 1 ? 0.5f : -0.5f) : 0.0f;
                require(impulse.right[i] == expected, "Stereo impulse latency or polarity is wrong");
            }
        }
    }
    std::cout << "Neutral startup, impulse, and delay: bit-exact at 8/44.1/48/96/192/384 kHz, mono/stereo\n";
}

void testNoiseAndTone() {
    SpectralCleaner engine; engine.prepare(sampleRate, 2); engine.setParameters(1.0f, 0.0f);
    auto untouched = white(latency * 4); const auto original = untouched;
    run(engine, untouched);
    for (std::size_t i = latency; i < untouched.size(); ++i)
        require(untouched.left[i] == original.left[i - latency] && untouched.right[i] == original.right[i - latency],
                "Unlearned denoiser modified audio");
    const auto learned = learn(engine);
    require(learned.noiseReady, "Noise state was not marked ready");
    auto output = white(static_cast<std::size_t>(sampleRate) + latency, 0x84526814U);
    const auto input = output; run(engine, output);
    const double reduction = attenuation(output, input, static_cast<std::size_t>(sampleRate * 0.4), static_cast<std::size_t>(sampleRate));
    std::cout << "Learned stationary white-noise attenuation: " << reduction << " dB\n";
    require(reduction < -8.0 && reduction > -24.5, "Learned noise attenuation is weak or exceeds its bounded floor");

    constexpr double hz = 441.0;
    constexpr float amplitude = 0.2f;
    Audio wanted(static_cast<std::size_t>(sampleRate) + latency), clean(wanted.size());
    Random random{0x98765432U};
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const float value = amplitude * static_cast<float>(std::sin(2.0 * pi * hz * static_cast<double>(i) / sampleRate));
        clean.left[i] = clean.right[i] = value;
        wanted.left[i] = value + 0.03f * random.noise();
        wanted.right[i] = value + 0.03f * random.noise();
    }
    const auto noisy = wanted; run(engine, wanted);
    double correlation = 0.0, cleanPower = 0.0, beforeError = 0.0, afterError = 0.0;
    for (std::size_t i = static_cast<std::size_t>(sampleRate * 0.35); i < static_cast<std::size_t>(sampleRate); ++i) {
        const double expected = clean.left[i], actual = wanted.left[i + latency];
        correlation += expected * actual;
        cleanPower += expected * expected;
        const double before = noisy.left[i] - expected, after = actual - expected;
        beforeError += before * before; afterError += after * after;
    }
    const double gainDb = 20.0 * std::log10(std::max(1.0e-30, correlation / cleanPower));
    const double errorImprovement = db(beforeError, afterError);
    std::cout << "Wanted 441 Hz tone gain: " << gainDb << " dB; error-to-clean improvement: " << errorImprovement << " dB\n";
    require(gainDb > -1.5 && gainDb < 0.25, "Denoiser removed the dominant wanted tone");
    require(errorImprovement > 4.0, "Noise suppression did not improve known-clean tone error");
}

void testOverlapAddGain() {
    // An intentionally oversized flat profile forces the documented 24 dB
    // gain floor. This checks the inverse FFT, OLA normalisation and alignment,
    // which the exact neutral residual path alone would not exercise.
    SpectralCleaner engine; engine.prepare(sampleRate, 2); engine.setParameters(1.0f, 0.0f);
    SpectralCleaner::LearnedState state; state.sampleRate = sampleRate; state.noiseReady = true; state.noisePower.fill(1.0e6f);
    engine.restoreLearnedState(state);
    Audio warm(static_cast<std::size_t>(sampleRate * 2.0)); run(engine, warm);
    auto output = white(static_cast<std::size_t>(sampleRate * 0.25) + latency, 0x71458923U, 0.4f); const auto input = output;
    run(engine, output);
    const double gain = std::pow(10.0, -24.0 / 20.0);
    double maximumError = 0.0;
    for (std::size_t i = 0; i + latency < output.size(); ++i) {
        maximumError = std::max({maximumError,
            std::abs(output.left[i + latency] - gain * input.left[i]),
            std::abs(output.right[i + latency] - gain * input.right[i])});
    }
    std::cout << "Flat -24 dB spectral OLA maximum absolute error: " << maximumError << "\n";
    require(maximumError < 2.0e-6, "Active spectral reconstruction has wrong gain, window sum, phase or latency");
}

void testStereoAndBlocks() {
    SpectralCleaner first, second; first.setParameters(0.8f, 0.55f); second.setParameters(0.8f, 0.55f);
    const auto state = learn(first); first.reset(); second.reset(); first.restoreLearnedState(state); second.restoreLearnedState(state);
    auto one = white(static_cast<std::size_t>(sampleRate * 0.5), 0x53465125U); auto many = one;
    run(first, one, 1); run(second, many, 509);
    require(one.left == many.left && one.right == many.right, "Spectral processing depends on host block size");
    first.reset(); first.restoreLearnedState(state);
    auto anti = white(static_cast<std::size_t>(sampleRate * 0.5), 0x35248128U);
    for (std::size_t i = 0; i < anti.size(); ++i) anti.right[i] = -anti.left[i];
    run(first, anti, 257);
    for (std::size_t i = 0; i < anti.size(); ++i)
        require(std::abs(anti.left[i] + anti.right[i]) < 2.0e-7f, "Linked spectral stages damaged anti-phase stereo");
    first.reset(); first.restoreLearnedState(state);
    auto balanced = white(static_cast<std::size_t>(sampleRate * 0.5), 0x73925184U);
    for (std::size_t i = 0; i < balanced.size(); ++i) balanced.right[i] = 0.25f * balanced.left[i];
    run(first, balanced);
    for (std::size_t i = 0; i < balanced.size(); ++i)
        require(std::abs(balanced.right[i] - 0.25f * balanced.left[i]) < 2.0e-7f, "Linked spectral stages changed stereo balance");
    std::cout << "Block grouping, anti-phase stereo, and stereo balance: passed\n";
}

void testProfileCaptureAndRecall() {
    SpectralCleaner dry, processed; dry.setParameters(0.0f, 0.0f); processed.setParameters(1.0f, 1.0f);
    dry.requestNoiseLearn(); processed.requestNoiseLearn();
    auto prefix = white(static_cast<std::size_t>(sampleRate * 1.9)); auto prefixProcessed = prefix;
    run(dry, prefix); run(processed, prefixProcessed);
    require(dry.getMeters().noiseLearning && !dry.getMeters().noiseProfileReady,
            "Noise learning completed before its requested two seconds");
    require(dry.getMeters().noiseLearnProgress > 0.94f && dry.getMeters().noiseLearnProgress < 0.96f,
            "Noise learning progress does not correspond to elapsed samples");
    auto suffix = white(static_cast<std::size_t>(sampleRate * 0.15), 0x84719823U); auto suffixProcessed = suffix;
    run(dry, suffix); run(processed, suffixProcessed);
    const auto state = dry.getLearnedState();
    require(state.noiseReady && state.noisePower == processed.getLearnedState().noisePower,
            "Profile capture depends on effect amounts instead of raw input spectrum");
    processed.restoreLearnedState(state);
    const auto restored = processed.getLearnedState();
    require(restored.noisePower == state.noisePower && restored.sampleRate == state.sampleRate && restored.noiseReady,
            "Noise profile did not survive exact state round trip");
    auto wanted = white(static_cast<std::size_t>(sampleRate * 0.3), 0x54896324U, 0.6f); run(processed, wanted);
    require(processed.getLearnedState().noisePower == state.noisePower, "Profile secretly adapted to wanted audio");
    auto invalid = state; invalid.noisePower[87] = std::numeric_limits<float>::quiet_NaN(); processed.restoreLearnedState(invalid);
    require(!processed.getMeters().noiseProfileReady, "Nonfinite noise profile was accepted");
    invalid = state; invalid.noisePower[42] = -1.0f; processed.restoreLearnedState(invalid);
    require(!processed.getMeters().noiseProfileReady, "Negative noise power was accepted");
    invalid = state; invalid.sampleRate = 44100.0; processed.restoreLearnedState(invalid);
    require(!processed.getMeters().noiseProfileReady, "Wrong sample-rate noise profile was accepted");
    processed.restoreLearnedState(state); processed.requestNoiseLearn();
    require(processed.getMeters().noiseLearning && !processed.getMeters().noiseProfileReady,
            "Relearning retained a misleading ready flag");
    processed.reset();
    require(!processed.getMeters().noiseLearning && !processed.getMeters().noiseProfileReady, "Reset did not clear learned state");
    std::cout << "Two-second explicit capture, raw-spectrum profile, exact recall and corrupt-state validation: passed\n";
}

void testNeutralReturnAndInvalidAudio() {
    SpectralCleaner engine; engine.setParameters(1.0f, 1.0f); learn(engine);
    engine.setParameters(1.0f, 1.0f, true);
    auto settling = white(static_cast<std::size_t>(sampleRate), 0x64518237U); run(engine, settling);
    auto output = white(latency * 4, 0x98165243U); const auto input = output; run(engine, output);
    for (std::size_t i = latency; i < output.size(); ++i)
        require(output.left[i] == input.left[i - latency] && output.right[i] == input.right[i - latency],
                "Settled spectral bypass did not return to exact delayed dry");
    engine.setParameters(1.0f, 1.0f);
    auto invalid = white(latency * 4, 0x36847291U);
    invalid.left[0] = std::numeric_limits<float>::infinity();
    invalid.right[1] = -std::numeric_limits<float>::infinity();
    invalid.left[2] = std::numeric_limits<float>::quiet_NaN();
    invalid.right[3] = std::numeric_limits<float>::max();
    invalid.left[4] = -std::numeric_limits<float>::max();
    engine.setParameters(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    run(engine, invalid);
    engine.reset(); Audio silence(latency * 3); run(engine, silence);
    for (std::size_t i = 0; i < silence.size(); ++i)
        require(silence.left[i] == 0.0f && silence.right[i] == 0.0f, "Reset left stale spectral audio in the output");
    std::cout << "Smoothed bypass endpoint, hostile input and reset: passed\n";
}

void testIdleWake() {
    SpectralCleaner engine;
    SpectralCleaner::LearnedState state; state.sampleRate = sampleRate; state.noiseReady = true;
    engine.restoreLearnedState(state);
    double maximumError = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(sampleRate * 3.0); ++i) {
        // Changes deliberately fall outside both hop and usual host boundaries.
        if (i == 16001) engine.setParameters(1.0f, 0.6f);
        if (i == 42071) engine.setParameters(1.0f, 0.6f, true);
        if (i == 84522) engine.setParameters(1.0f, 0.6f, false);
        if (i == 116003) engine.setParameters(0.0f, 0.0f);
        float left = 0.25f, right = -0.1f;
        engine.processSample(left, right, 2);
        const float expectedLeft = i < latency ? 0.0f : 0.25f;
        const float expectedRight = i < latency ? 0.0f : -0.1f;
        maximumError = std::max({maximumError, std::abs(static_cast<double>(left - expectedLeft)),
                                std::abs(static_cast<double>(right - expectedRight))});
    }
    std::cout << "Idle/wake/bypass DC continuity maximum error: " << maximumError << "\n";
    require(maximumError < 2.0e-5, "Sleeping spectral processing left a gap or stale history on re-enable");
}

void testNoAudioAllocation() {
    SpectralCleaner engine; engine.prepare(sampleRate, 2); engine.setParameters(0.9f, 0.6f);
    auto audio = white(static_cast<std::size_t>(sampleRate * 2.2));
    allocationCount.store(0); tracking.store(true);
    engine.requestNoiseLearn();
    for (std::size_t i = 0; i < audio.size(); ++i) {
        if (i % 8191 == 0) engine.setParameters(0.65f + static_cast<float>(i % 5) * 0.05f, 0.75f);
        engine.processSample(audio.left[i], audio.right[i], 2);
    }
    tracking.store(false);
    require(allocationCount.load() == 0, "C++ heap allocation occurred in spectral audio processing");
    std::cout << "Audio-path C++ allocations, including learning and amount changes: zero\n";
}
}

int main() {
    try {
        testNeutralDelay();
        testNoiseAndTone();
        testOverlapAddGain();
        testStereoAndBlocks();
        testProfileCaptureAndRecall();
        testNeutralReturnAndInvalidAudio();
        testIdleWake();
        testNoAudioAllocation();
        std::cout << "All spectral cleaner tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        tracking.store(false);
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}

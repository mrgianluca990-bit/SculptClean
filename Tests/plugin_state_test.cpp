#include "PluginProcessor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
constexpr int latency = 2051;
constexpr int bins = 1025;
constexpr int blockSize = 128;
constexpr std::array<const char*, 11> parameterIds {
    "denoise", "dereverb", "click", "hum", "humFrequency", "sourceThreshold",
    "sourceDepth", "sourceEnabled", "listenRemoved", "output", "bypass"
};

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
void near(float got, float expected, const char* text) {
    require(std::isfinite(got) && std::abs(got - expected) < 0.001f, text);
}
float value(SculptCleanProcessor& p, const char* id) {
    auto* raw = p.apvts.getRawParameterValue(id);
    require(raw != nullptr, "Missing APVTS parameter");
    return raw->load(std::memory_order_relaxed);
}
void set(SculptCleanProcessor& p, const char* id, float raw) {
    auto* parameter = p.apvts.getParameter(id);
    require(parameter != nullptr, "Missing parameter for write");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(raw));
}
juce::ValueTree snapshot(SculptCleanProcessor& p) {
    juce::MemoryBlock binary;
    p.getStateInformation(binary);
    auto xml = juce::AudioProcessor::getXmlFromBinary(binary.getData(), static_cast<int>(binary.getSize()));
    require(xml != nullptr, "Invalid state XML");
    auto tree = juce::ValueTree::fromXml(*xml);
    require(tree.hasType("SCULPT_CLEAN_STATE"), "Wrong product state identifier");
    require(static_cast<int>(tree["schemaVersion"]) == 1, "State schema version changed");
    return tree;
}
void restore(SculptCleanProcessor& p, const juce::ValueTree& tree) {
    auto xml = tree.createXml();
    require(xml != nullptr, "Cannot encode state XML");
    juce::MemoryBlock binary;
    juce::AudioProcessor::copyXmlToBinary(*xml, binary);
    p.setStateInformation(binary.getData(), static_cast<int>(binary.getSize()));
}
float profilePower(int bin, float scale) {
    return scale * (1.0e-7f * static_cast<float>(bin + 1));
}
void addLearned(juce::ValueTree& tree, float scale = 1.0f, double sampleRate = 48000.0) {
    tree.removeChild(tree.getChildWithName("LEARNED"), nullptr);
    juce::ValueTree learned("LEARNED");
    learned.setProperty("fftSize", 2048, nullptr);
    learned.setProperty("sampleRate", sampleRate, nullptr);
    learned.setProperty("noiseReady", true, nullptr);
    for (int bin = 0; bin < bins; ++bin)
        learned.setProperty("noise" + juce::String(bin), static_cast<double>(profilePower(bin, scale)), nullptr);
    tree.addChild(learned, -1, nullptr);
}
void verifyLearned(const juce::ValueTree& tree, float scale = 1.0f) {
    const auto learned = tree.getChildWithName("LEARNED");
    require(learned.isValid() && static_cast<bool>(learned["noiseReady"]), "Lost learned noise readiness");
    require(static_cast<int>(learned["fftSize"]) == 2048, "Lost FFT size in learned state");
    near(static_cast<float>(learned["sampleRate"]), 48000.0f, "Changed learned profile sample rate");
    for (int bin = 0; bin < bins; ++bin) {
        const juce::Identifier key("noise" + juce::String(bin));
        require(learned.hasProperty(key), "Lost a noise-profile bin");
        const float expected = profilePower(bin, scale);
        const float got = static_cast<float>(learned[key]);
        require(std::isfinite(got) && std::abs(got - expected) < std::max(1.0e-12f, expected * 1.0e-6f),
                "Changed one of the 1025 learned noise powers");
    }
}
void verifyUnready(const juce::ValueTree& tree, bool requireZeroPower) {
    const auto learned = tree.getChildWithName("LEARNED");
    require(learned.isValid() && !static_cast<bool>(learned["noiseReady"]), "Invalid profile was marked ready");
    for (int bin = 0; bin < bins; ++bin) {
        const float power = static_cast<float>(learned["noise" + juce::String(bin)]);
        require(std::isfinite(power) && power >= 0.0f, "Saved profile contains invalid noise power");
        if (requireZeroPower) require(power == 0.0f, "Pending Learn retained the previous profile");
    }
}
void processSilence(SculptCleanProcessor& p, int blocks = 1) {
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    for (int block = 0; block < blocks; ++block) {
        audio.clear();
        p.processBlock(audio, midi);
    }
}
void stateValue(juce::ValueTree& tree, const char* id, const juce::var& raw) {
    auto child = tree.getChildWithProperty("id", id);
    require(child.isValid(), "Missing state parameter to edit");
    child.setProperty("value", raw, nullptr);
}

void defaultsAndOrder() {
    SculptCleanProcessor p;
    near(value(p, "denoise"), 35.0f, "Denoise default changed");
    near(value(p, "sourceEnabled"), 0.0f, "Extractor must default off");
    near(value(p, "sourceThreshold"), -32.0f, "Threshold default changed");
    near(value(p, "sourceDepth"), 50.0f, "Depth default changed");
    near(value(p, "humFrequency"), 0.0f, "Hum must default to the 50 Hz choice");
    near(value(p, "output"), 0.0f, "Output must default to unity");
    require(p.getLatencySamples() == latency, "Latency must be known before prepare");
    require(p.getBypassParameter() == p.apvts.getParameter("bypass"), "Host bypass parameter is not exposed");
    require(p.getParameters().size() == static_cast<int>(parameterIds.size()), "Unexpected parameter count");
    for (std::size_t i = 0; i < parameterIds.size(); ++i) {
        const auto* ranged = dynamic_cast<const juce::AudioProcessorParameterWithID*>(p.getParameters()[static_cast<int>(i)]);
        require(ranged != nullptr && ranged->paramID == parameterIds[i], "Automation parameter order changed");
    }
    verifyUnready(snapshot(p), true);
}

void recallAndStoppedState() {
    SculptCleanProcessor a, b, c;
    const std::array<float, parameterIds.size()> settings {
        39.1f, 23.4f, 12.3f, 45.6f, 1.0f, -27.3f, 67.4f, 1.0f, 1.0f, -2.3f, 1.0f
    };
    for (std::size_t i = 0; i < parameterIds.size(); ++i) set(a, parameterIds[i], settings[i]);
    auto saved = snapshot(a);
    addLearned(saved);
    restore(b, saved);
    for (std::size_t i = 0; i < parameterIds.size(); ++i)
        near(value(b, parameterIds[i]), settings[i], "Parameter recall failed");
    verifyLearned(snapshot(b)); // A stopped host can immediately save the pending restore.
    verifyLearned(snapshot(b)); // Reading state must not consume the pending command.
    b.prepareToPlay(48000.0, blockSize);
    require(b.display.noiseProfileReady.load(), "Prepare did not apply pending learned profile");
    near(b.display.latencyMilliseconds.load(), 1000.0f * latency / 48000.0f, "Displayed latency is incorrect");
    processSilence(b);
    verifyLearned(snapshot(b));
    b.releaseResources();
    verifyLearned(snapshot(b));
    b.prepareToPlay(48000.0, blockSize);
    verifyLearned(snapshot(b));

    // Replace an already active profile while transport is stopped. The save
    // must contain the new pending profile, never the last audio publication.
    auto replacement = saved.createCopy();
    addLearned(replacement, 3.0f);
    stateValue(replacement, "sourceThreshold", -19.7f);
    restore(b, replacement);
    const auto stoppedSave = snapshot(b);
    verifyLearned(stoppedSave, 3.0f);
    restore(c, stoppedSave);
    verifyLearned(snapshot(c), 3.0f);
    near(value(c, "sourceThreshold"), -19.7f, "Stopped restore/save lost parameter changes");
    c.prepareToPlay(48000.0, blockSize);
    processSilence(c);
    verifyLearned(snapshot(c), 3.0f);
    processSilence(b);
    verifyLearned(snapshot(b), 3.0f);
}

void learnWhileStopped() {
    SculptCleanProcessor p;
    auto saved = snapshot(p);
    addLearned(saved);
    restore(p, saved);
    p.requestNoiseLearn(); // Pending restore and pending Learn before first prepare.
    verifyUnready(snapshot(p), true);
    verifyUnready(snapshot(p), true);
    p.prepareToPlay(48000.0, blockSize);
    require(p.display.noiseLearning.load(), "Saving while stopped consumed pending Learn intent");
    require(!p.display.noiseProfileReady.load(), "Learn did not invalidate the previously loaded profile");
    processSilence(p);
    require(p.display.noiseLearning.load(), "Learn was not running after processing resumed");

    // A state saved during a new Learn restores unready, avoiding accidental
    // capture of musical content when reopening a project.
    SculptCleanProcessor reopened;
    restore(reopened, snapshot(p));
    reopened.prepareToPlay(48000.0, blockSize);
    require(!reopened.display.noiseProfileReady.load(), "Reopening revived the old profile");
    require(!reopened.display.noiseLearning.load(), "Session load unexpectedly began a noise capture");

    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    std::uint32_t seed = 0x12345678U;
    for (int block = 0; block < 760; ++block) {
        for (int n = 0; n < blockSize; ++n) {
            seed = seed * 1664525U + 1013904223U;
            const float sample = 0.002f * (static_cast<float>(seed >> 8U) / 16777215.0f - 0.5f);
            audio.setSample(0, n, sample);
            audio.setSample(1, n, -sample * 0.75f);
        }
        p.processBlock(audio, midi);
    }
    require(!p.display.noiseLearning.load() && p.display.noiseProfileReady.load(), "Two-second Learn did not publish a finished profile");
    const auto learned = snapshot(p).getChildWithName("LEARNED");
    require(static_cast<bool>(learned["noiseReady"]), "Completed Learn did not reach saved state");
    bool positivePower = false;
    for (int bin = 0; bin < bins; ++bin) {
        const float power = static_cast<float>(learned["noise" + juce::String(bin)]);
        require(std::isfinite(power) && power >= 0.0f, "Completed Learn produced invalid power");
        positivePower = positivePower || power > 0.0f;
    }
    require(positivePower, "Completed Learn saved an empty noise profile");

    // Repeat after a previous audio publication, without another prepare.
    p.requestNoiseLearn();
    verifyUnready(snapshot(p), true);
    processSilence(p);
    require(p.display.noiseLearning.load() && !p.display.noiseProfileReady.load(), "Stopped re-Learn command was lost");
}

void malformedAndPartialState() {
    SculptCleanProcessor p;
    set(p, "sourceEnabled", 1.0f);
    set(p, "sourceThreshold", -12.0f);
    set(p, "sourceDepth", 98.0f);
    set(p, "listenRemoved", 1.0f);
    set(p, "humFrequency", 1.0f);
    auto partial = snapshot(p);
    addLearned(partial);
    for (const char* id : { "sourceEnabled", "sourceThreshold", "sourceDepth", "listenRemoved", "humFrequency" })
        partial.removeChild(partial.getChildWithProperty("id", id), nullptr);
    restore(p, partial);
    near(value(p, "sourceEnabled"), 0.0f, "Partial state retained enabled source");
    near(value(p, "sourceThreshold"), -32.0f, "Partial state retained an unrelated threshold");
    near(value(p, "sourceDepth"), 50.0f, "Partial state retained unrelated source depth");
    near(value(p, "listenRemoved"), 0.0f, "Partial state retained removed-signal monitoring");
    near(value(p, "humFrequency"), 0.0f, "Partial state did not default to 50 Hz");
    verifyLearned(snapshot(p));

    auto outOfRange = snapshot(p);
    stateValue(outOfRange, "denoise", 999.0f);
    stateValue(outOfRange, "sourceThreshold", -999.0f);
    stateValue(outOfRange, "sourceDepth", 999.0f);
    stateValue(outOfRange, "output", 999.0f);
    stateValue(outOfRange, "humFrequency", -99.0f);
    stateValue(outOfRange, "bypass", 4.0f);
    restore(p, outOfRange);
    near(value(p, "denoise"), 100.0f, "Denoise was not clamped");
    near(value(p, "sourceThreshold"), -60.0f, "Threshold was not clamped");
    near(value(p, "sourceDepth"), 100.0f, "Source depth was not clamped");
    near(value(p, "output"), 6.0f, "Output gain was not clamped");
    near(value(p, "humFrequency"), 0.0f, "Invalid hum choice was not clamped");
    near(value(p, "bypass"), 1.0f, "Boolean parameter was not normalized");

    // A non-finite parameter rejects the whole state, including earlier valid
    // fields, so a corrupt preset cannot leave a half-applied configuration.
    auto invalid = snapshot(p);
    stateValue(invalid, "denoise", 12.0f);
    stateValue(invalid, "sourceDepth", "nan");
    restore(p, invalid);
    near(value(p, "denoise"), 100.0f, "Corrupt state was partially applied");
    near(value(p, "sourceDepth"), 100.0f, "Non-finite source depth entered APVTS");
    verifyLearned(snapshot(p));

    auto badProfile = snapshot(p);
    badProfile.getChildWithName("LEARNED").setProperty("noise1024", -1.0f, nullptr);
    restore(p, badProfile);
    verifyUnready(snapshot(p), false);
    badProfile = partial.createCopy();
    badProfile.getChildWithName("LEARNED").removeProperty("noise512", nullptr);
    restore(p, badProfile);
    verifyUnready(snapshot(p), false);
    badProfile = partial.createCopy();
    badProfile.getChildWithName("LEARNED").setProperty("sampleRate", "nan", nullptr);
    restore(p, badProfile);
    verifyUnready(snapshot(p), false);
    p.prepareToPlay(48000.0, blockSize);
    processSilence(p);
    require(!p.display.noiseProfileReady.load(), "Invalid noise profile reached the engine");
}

void sampleRateChange() {
    SculptCleanProcessor p, pending;
    auto saved = snapshot(p);
    addLearned(saved);
    restore(p, saved);
    p.prepareToPlay(48000.0, blockSize);
    verifyLearned(snapshot(p));
    p.releaseResources();
    p.prepareToPlay(44100.0, blockSize);
    require(p.getLatencySamples() == latency, "Sample-rate change altered fixed sample latency");
    near(p.display.latencyMilliseconds.load(), 1000.0f * latency / 44100.0f, "Sample-rate change did not update latency display");
    require(!p.display.noiseProfileReady.load(), "Sample-rate change retained incompatible frequency bins");
    verifyUnready(snapshot(p), true);
    p.prepareToPlay(48000.0, blockSize);
    verifyUnready(snapshot(p), true); // Returning to the old rate must not resurrect a discarded profile.

    restore(pending, saved);
    verifyLearned(snapshot(pending));
    pending.prepareToPlay(44100.0, blockSize);
    verifyUnready(snapshot(pending), true); // Pending save logic must not republish rejected old-rate data.
}

void hostBypassAndLatency() {
    SculptCleanProcessor p;
    for (const char* id : { "denoise", "dereverb", "click", "hum" }) set(p, id, 0.0f);
    set(p, "sourceEnabled", 1.0f);
    set(p, "sourceThreshold", -20.0f);
    set(p, "sourceDepth", 100.0f);
    set(p, "output", 6.0f);
    set(p, "listenRemoved", 1.0f);
    p.prepareToPlay(48000.0, blockSize);
    require(p.getLatencySamples() == latency, "Incorrect host compensation latency");
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    const auto fillQuiet = [&] {
        for (int n = 0; n < blockSize; ++n) {
            audio.setSample(0, n, 0.001f);
            audio.setSample(1, n, -0.0005f);
        }
    };
    for (int block = 0; block < 375; ++block) { fillQuiet(); p.processBlock(audio, midi); }
    require(p.display.sourceReductionDb.load() > 10.0f, "Wrapper did not expose Source Extractor reduction");
    std::array<float, parameterIds.size()> before {};
    for (std::size_t i = 0; i < parameterIds.size(); ++i) before[i] = value(p, parameterIds[i]);
    // Allow the 5 ms click-free transition to reach the dry endpoint.
    for (int block = 0; block < 8; ++block) { fillQuiet(); p.processBlockBypassed(audio, midi); }

    std::array<std::array<float, latency>, 2> history {};
    history[0].fill(0.001f);
    history[1].fill(-0.0005f);
    std::array<std::array<float, blockSize>, 2> expected {};
    std::size_t writePosition = 0;
    for (int block = 0; block < 40; ++block) {
        for (int n = 0; n < blockSize; ++n) {
            const int t = block * blockSize + n;
            const float left = 0.1f * std::sin(0.013f * static_cast<float>(t));
            const float right = 0.07f * std::cos(0.029f * static_cast<float>(t));
            expected[0][static_cast<std::size_t>(n)] = history[0][writePosition];
            expected[1][static_cast<std::size_t>(n)] = history[1][writePosition];
            history[0][writePosition] = left;
            history[1][writePosition] = right;
            writePosition = (writePosition + 1U) % static_cast<std::size_t>(latency);
            audio.setSample(0, n, left);
            audio.setSample(1, n, right);
        }
        p.processBlockBypassed(audio, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int n = 0; n < blockSize; ++n)
                require(audio.getSample(channel, n) == expected[static_cast<std::size_t>(channel)][static_cast<std::size_t>(n)],
                        "Host bypass was not bit-exact input delayed by 2051 samples");
    }
    for (std::size_t i = 0; i < parameterIds.size(); ++i)
        near(value(p, parameterIds[i]), before[i], "Host bypass modified a saved parameter");

    // Startup impulse checks initial delay and channel mapping independently
    // of steady-state delay history and the Source Extractor envelope.
    SculptCleanProcessor startup;
    set(startup, "sourceEnabled", 1.0f);
    set(startup, "sourceDepth", 100.0f);
    set(startup, "output", 6.0f);
    startup.prepareToPlay(48000.0, blockSize);
    for (int block = 0; block < 40; ++block) {
        audio.clear();
        if (block == 0) { audio.setSample(0, 0, 0.25f); audio.setSample(1, 0, -0.125f); }
        startup.processBlockBypassed(audio, midi);
        for (int n = 0; n < blockSize; ++n) {
            const bool atImpulse = block * blockSize + n == latency;
            require(audio.getSample(0, n) == (atImpulse ? 0.25f : 0.0f), "Startup left bypass impulse has wrong latency");
            require(audio.getSample(1, n) == (atImpulse ? -0.125f : 0.0f), "Startup right bypass impulse has wrong latency");
        }
    }
    near(value(startup, "bypass"), 0.0f, "Host bypass changed the startup bypass parameter");
}
} // namespace

int main() {
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    try {
        defaultsAndOrder();
        std::cout << "PASS: defaults, automation order and reported latency\n";
        recallAndStoppedState();
        std::cout << "PASS: all parameters, 1025-bin profile and stopped restore/save recall\n";
        learnWhileStopped();
        std::cout << "PASS: stopped Learn intent, old-profile invalidation and completed capture publication\n";
        malformedAndPartialState();
        std::cout << "PASS: missing parameters, bounded values and corrupt state rejection\n";
        sampleRateChange();
        std::cout << "PASS: sample-rate changes invalidate incompatible learned profiles\n";
        hostBypassAndLatency();
        std::cout << "PASS: Source Extractor meters and bit-exact 2051-sample host bypass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

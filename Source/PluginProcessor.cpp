#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <memory>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

static_assert (std::atomic<float>::is_always_lock_free && std::atomic<double>::is_always_lock_free
               && std::atomic<std::uint64_t>::is_always_lock_free,
               "Sculpt Clean requires lock-free scalar atomics on its supported 64-bit targets");

void SculptCleanProcessor::AtomicLearnedState::store (const sculptclean::LearnedState& value,
                                                     std::uint64_t learnGeneration,
                                                     std::uint64_t restoreGeneration) noexcept
{
    sequence.fetch_add (1, std::memory_order_acq_rel);
    std::atomic_thread_fence (std::memory_order_release);
    for (std::size_t i = 0; i < noisePower.size(); ++i)
        noisePower[i].store (value.noisePower[i], std::memory_order_relaxed);
    noiseReady.store (value.noiseReady, std::memory_order_relaxed);
    sampleRate.store (value.sampleRate, std::memory_order_relaxed);
    consumedLearnGeneration.store (learnGeneration, std::memory_order_relaxed);
    consumedRestoreGeneration.store (restoreGeneration, std::memory_order_relaxed);
    sequence.fetch_add (1, std::memory_order_release);
}

bool SculptCleanProcessor::AtomicLearnedState::load (sculptclean::LearnedState& value,
    std::uint64_t& version, std::uint64_t* learnGeneration, std::uint64_t* restoreGeneration) const noexcept
{
    const auto before = sequence.load (std::memory_order_acquire);
    if ((before & 1U) != 0) return false;
    for (std::size_t i = 0; i < noisePower.size(); ++i)
        value.noisePower[i] = noisePower[i].load (std::memory_order_relaxed);
    value.noiseReady = noiseReady.load (std::memory_order_relaxed);
    value.sampleRate = sampleRate.load (std::memory_order_relaxed);
    const auto learned = consumedLearnGeneration.load (std::memory_order_relaxed);
    const auto restored = consumedRestoreGeneration.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    const auto after = sequence.load (std::memory_order_relaxed);
    if (before != after) return false;
    version = after;
    if (learnGeneration != nullptr) *learnGeneration = learned;
    if (restoreGeneration != nullptr) *restoreGeneration = restored;
    return true;
}

juce::AudioProcessorValueTreeState::ParameterLayout SculptCleanProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout parameters;
    const auto percent = AudioParameterFloatAttributes().withLabel ("%");
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "denoise", 1 }, "Denoise", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 35.0f, percent));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "dereverb", 1 }, "Dereverb", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "click", 1 }, "De-click", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "hum", 1 }, "De-hum", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    parameters.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "humFrequency", 1 }, "Hum Frequency", StringArray { "50 Hz", "60 Hz" }, 0));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "sourceThreshold", 1 }, "Source Threshold", NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -32.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "sourceDepth", 1 }, "Source Depth", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent));
    parameters.add (std::make_unique<AudioParameterBool> (ParameterID { "sourceEnabled", 1 }, "Source Enabled", false));
    parameters.add (std::make_unique<AudioParameterBool> (ParameterID { "listenRemoved", 1 }, "Listen Removed", false));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-12.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    parameters.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return parameters;
}

SculptCleanProcessor::SculptCleanProcessor()
    : AudioProcessor (BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SCULPT_CLEAN_STATE", createLayout())
{
    denoiseParameter = apvts.getRawParameterValue ("denoise");
    dereverbParameter = apvts.getRawParameterValue ("dereverb");
    clickParameter = apvts.getRawParameterValue ("click");
    humParameter = apvts.getRawParameterValue ("hum");
    humFrequencyParameter = apvts.getRawParameterValue ("humFrequency");
    outputParameter = apvts.getRawParameterValue ("output");
    bypassParameter = apvts.getRawParameterValue ("bypass");
    sourceThresholdParameter = apvts.getRawParameterValue ("sourceThreshold");
    sourceDepthParameter = apvts.getRawParameterValue ("sourceDepth");
    sourceEnabledParameter = apvts.getRawParameterValue ("sourceEnabled");
    listenRemovedParameter = apvts.getRawParameterValue ("listenRemoved");
    clearDisplay();
    setLatencySamples (2051);
}

bool SculptCleanProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto input = layout.getMainInputChannelSet();
    return (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo())
        && layout.getMainOutputChannelSet() == input;
}

sculptclean::Parameters SculptCleanProcessor::readParameters (bool hostBypass) const noexcept
{
    sculptclean::Parameters result;
    result.denoise = denoiseParameter->load (std::memory_order_relaxed) * 0.01f;
    result.dereverb = dereverbParameter->load (std::memory_order_relaxed) * 0.01f;
    result.click = clickParameter->load (std::memory_order_relaxed) * 0.01f;
    result.hum = humParameter->load (std::memory_order_relaxed) * 0.01f;
    result.humHz = humFrequencyParameter->load (std::memory_order_relaxed) >= 0.5f ? 60.0f : 50.0f;
    result.sourceThresholdDb = sourceThresholdParameter->load (std::memory_order_relaxed);
    result.sourceDepth = sourceDepthParameter->load (std::memory_order_relaxed) * 0.01f;
    result.sourceEnabled = sourceEnabledParameter->load (std::memory_order_relaxed) >= 0.5f;
    result.listenRemoved = listenRemovedParameter->load (std::memory_order_relaxed) >= 0.5f;
    result.outputGain = juce::Decibels::decibelsToGain (outputParameter->load (std::memory_order_relaxed));
    result.bypass = hostBypass || bypassParameter->load (std::memory_order_relaxed) >= 0.5f;
    return result;
}

void SculptCleanProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    const auto previous = engine.getLearnedState();
    engine.setParameters (readParameters (false));
    engine.prepare (sampleRate, std::max (1, maximumBlockSize), getTotalNumOutputChannels());
    if (! applyPendingLearnedState()) engine.restoreLearnedState (previous);
    (void) consumeCommands();
    setLatencySamples (engine.getLatencySamples());
    const auto validRate = std::isfinite (sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
        ? sampleRate : 48000.0;
    display.latencyMilliseconds.store (static_cast<float> (1000.0 * getLatencySamples() / validRate), std::memory_order_relaxed);
    clearDisplay();
    publishMeters();
    publishLearnedState();
}

void SculptCleanProcessor::releaseResources()
{
    const auto previous = engine.getLearnedState();
    engine.reset();
    engine.restoreLearnedState (previous);
    clearDisplay();
    publishMeters();
    publishLearnedState();
}

bool SculptCleanProcessor::applyPendingLearnedState() noexcept
{
    const auto candidate = pendingLearnedState.sequence.load (std::memory_order_acquire);
    if (candidate == 0 || candidate == appliedRestoreVersion || (candidate & 1U) != 0) return false;
    sculptclean::LearnedState incoming;
    std::uint64_t version = 0, learnAtRestore = 0;
    if (! pendingLearnedState.load (incoming, version, &learnAtRestore)
        || version == 0 || version == appliedRestoreVersion) return false;
    engine.restoreLearnedState (incoming);
    consumedLearnGeneration = learnAtRestore;
    appliedRestoreVersion = version;
    return true;
}

void SculptCleanProcessor::publishLearnedState() noexcept
{
    const auto state = engine.getLearnedState();
    const auto next = completedPublishedSlot.load (std::memory_order_relaxed) ^ 1U;
    publishedLearnedStates[next].store (state, consumedLearnGeneration, appliedRestoreVersion);
    completedPublishedSlot.store (next, std::memory_order_release);
    lastPublishedNoiseReady = state.noiseReady;
}

bool SculptCleanProcessor::consumeCommands() noexcept
{
    const auto learn = learnRequestGeneration.load (std::memory_order_relaxed);
    if (learn == consumedLearnGeneration) return false;
    engine.requestNoiseLearn();
    consumedLearnGeneration = learn;
    return true;
}

void SculptCleanProcessor::clearDisplay() noexcept
{
    display.inputPeak.store (0.0f, std::memory_order_relaxed);
    display.outputPeak.store (0.0f, std::memory_order_relaxed);
    display.denoiseReductionDb.store (0.0f, std::memory_order_relaxed);
    display.dereverbReductionDb.store (0.0f, std::memory_order_relaxed);
    display.sourceReductionDb.store (0.0f, std::memory_order_relaxed);
    display.noiseLearnProgress.store (0.0f, std::memory_order_relaxed);
    display.noiseLearning.store (false, std::memory_order_relaxed);
    display.noiseProfileReady.store (false, std::memory_order_relaxed);
}

void SculptCleanProcessor::publishMeters() noexcept
{
    const auto& meters = engine.getMeters();
    display.inputPeak.store (meters.inputPeak, std::memory_order_relaxed);
    display.outputPeak.store (meters.outputPeak, std::memory_order_relaxed);
    display.denoiseReductionDb.store (meters.denoiseReductionDb, std::memory_order_relaxed);
    display.dereverbReductionDb.store (meters.dereverbReductionDb, std::memory_order_relaxed);
    display.sourceReductionDb.store (meters.sourceReductionDb, std::memory_order_relaxed);
    display.noiseLearnProgress.store (meters.noiseLearnProgress, std::memory_order_relaxed);
    display.noiseLearning.store (meters.noiseLearning, std::memory_order_relaxed);
    display.noiseProfileReady.store (meters.noiseProfileReady, std::memory_order_relaxed);
}

void SculptCleanProcessor::process (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi, bool hostBypass)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    for (int channel = getTotalNumInputChannels(); channel < audio.getNumChannels(); ++channel)
        audio.clear (channel, 0, audio.getNumSamples());
    const int channels = std::min ({2, audio.getNumChannels(), getTotalNumOutputChannels()});
    if (channels <= 0) return;
    engine.setParameters (readParameters (hostBypass));
    const bool restored = applyPendingLearnedState();
    const bool learned = consumeCommands();
    engine.process (audio.getArrayOfWritePointers(), channels, audio.getNumSamples());
    publishMeters();
    // A 1025-bin profile is copied only for commands, completion or lifecycle
    // changes. Ordinary callbacks publish the small meters alone.
    if (restored || learned || engine.getMeters().noiseProfileReady != lastPublishedNoiseReady)
        publishLearnedState();
}

void SculptCleanProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    process (audio, midi, false);
}

void SculptCleanProcessor::processBlockBypassed (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    // Host bypass preserves the declared 2051-sample delay and never edits
    // saved parameters; only the core's latency-matched dry path is selected.
    process (audio, midi, true);
}

juce::AudioProcessorParameter* SculptCleanProcessor::getBypassParameter() const { return apvts.getParameter ("bypass"); }
juce::AudioProcessorEditor* SculptCleanProcessor::createEditor() { return new SculptCleanEditor (*this); }

void SculptCleanProcessor::getStateInformation (juce::MemoryBlock& data)
{
    const std::lock_guard<std::mutex> stateGuard (stateIoMutex);
    sculptclean::LearnedState captured;
    std::uint64_t version = 0;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto slot = completedPublishedSlot.load (std::memory_order_acquire);
        if (publishedLearnedStates[slot].load (captured, version,
                &lastSerializedLearnGeneration, &lastSerializedRestoreGeneration))
        {
            lastSerializedLearnedState = captured;
            break;
        }
    }
    std::uint64_t learnAtRestore = 0;
    if (pendingLearnedState.load (captured, version, &learnAtRestore)
        && version != 0 && version != lastSerializedRestoreGeneration)
    {
        lastSerializedLearnedState = captured;
        lastSerializedLearnGeneration = learnAtRestore;
    }
    if (learnRequestGeneration.load (std::memory_order_relaxed) != lastSerializedLearnGeneration)
    {
        lastSerializedLearnedState.noiseReady = false;
        lastSerializedLearnedState.noisePower.fill (0.0f);
    }
    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", 1, nullptr);
    juce::ValueTree learned ("LEARNED");
    learned.setProperty ("fftSize", 2048, nullptr);
    learned.setProperty ("sampleRate", lastSerializedLearnedState.sampleRate, nullptr);
    learned.setProperty ("noiseReady", lastSerializedLearnedState.noiseReady, nullptr);
    for (std::size_t i = 0; i < lastSerializedLearnedState.noisePower.size(); ++i)
        learned.setProperty ("noise" + juce::String (static_cast<int> (i)),
                             static_cast<double> (lastSerializedLearnedState.noisePower[i]), nullptr);
    state.addChild (learned, -1, nullptr);
    if (const auto xml = state.createXml()) copyXmlToBinary (*xml, data);
}

void SculptCleanProcessor::setStateInformation (const void* data, int size)
{
    const std::lock_guard<std::mutex> stateGuard (stateIoMutex);
    if (const auto xml = getXmlFromBinary (data, size))
    {
        if (! xml->hasTagName (apvts.state.getType())) return;
        auto state = juce::ValueTree::fromXml (*xml);
        if (! state.isValid()) return;
        sculptclean::LearnedState restored;
        if (const auto learned = state.getChildWithName ("LEARNED"); learned.isValid())
        {
            restored.sampleRate = static_cast<double> (learned["sampleRate"]);
            restored.noiseReady = static_cast<bool> (learned["noiseReady"])
                && static_cast<int> (learned["fftSize"]) == 2048;
            for (std::size_t i = 0; i < restored.noisePower.size(); ++i)
            {
                const juce::Identifier key ("noise" + juce::String (static_cast<int> (i)));
                const float power = static_cast<float> (learned[key]);
                if (! learned.hasProperty (key) || ! std::isfinite (power) || power < 0.0f)
                    restored.noiseReady = false;
                restored.noisePower[i] = std::isfinite (power) && power >= 0.0f ? power : 0.0f;
            }
            if (! std::isfinite (restored.sampleRate) || restored.sampleRate < 8000.0 || restored.sampleRate > 384000.0)
            {
                restored.noiseReady = false;
                restored.sampleRate = 48000.0;
            }
            state.removeChild (learned, nullptr);
        }
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            auto child = state.getChild (i);
            if (auto* parameter = apvts.getParameter (child["id"].toString()))
            {
                const float value = static_cast<float> (child["value"]);
                if (! std::isfinite (value)) return;
                const auto& range = parameter->getNormalisableRange();
                child.setProperty ("value", range.snapToLegalValue (juce::jlimit (range.start, range.end, value)), nullptr);
            }
        }
        // Partial states restore defaults for absent parameters instead of
        // retaining values from an unrelated previously loaded preset.
        for (auto* base : getParameters())
        {
            if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (base))
            {
                if (! state.getChildWithProperty ("id", parameter->paramID).isValid())
                {
                    juce::ValueTree child ("PARAM");
                    child.setProperty ("id", parameter->paramID, nullptr);
                    child.setProperty ("value", parameter->convertFrom0to1 (parameter->getDefaultValue()), nullptr);
                    state.addChild (child, -1, nullptr);
                }
            }
        }
        apvts.replaceState (state);
        lastSerializedLearnedState = restored;
        pendingLearnedState.store (restored, learnRequestGeneration.load (std::memory_order_relaxed), 0);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SculptCleanProcessor(); }

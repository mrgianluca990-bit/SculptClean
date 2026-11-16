#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "CleanEngine.h"

class SculptCleanProcessor final : public juce::AudioProcessor
{
public:
    SculptCleanProcessor();
    ~SculptCleanProcessor() override = default;

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Sculpt Clean"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    void requestNoiseLearn() noexcept { learnRequestGeneration.fetch_add (1, std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    struct DisplayState
    {
        std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
        std::atomic<float> denoiseReductionDb { 0.0f }, dereverbReductionDb { 0.0f };
        std::atomic<float> sourceReductionDb { 0.0f }, noiseLearnProgress { 0.0f };
        std::atomic<float> latencyMilliseconds { 2051000.0f / 48000.0f };
        std::atomic<bool> noiseLearning { false }, noiseProfileReady { false };
    } display;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    sculptclean::Parameters readParameters (bool hostBypass) const noexcept;
    void process (juce::AudioBuffer<float>&, juce::MidiBuffer&, bool hostBypass);
    void publishMeters() noexcept;
    void clearDisplay() noexcept;
    bool applyPendingLearnedState() noexcept;
    void publishLearnedState() noexcept;
    bool consumeCommands() noexcept;

    // Profile transfers are infrequent, bounded and lock-free on the audio
    // thread. Every field is atomic; the seqlock never races on plain memory.
    struct AtomicLearnedState
    {
        AtomicLearnedState() noexcept
        {
            for (auto& value : noisePower) value.store (0.0f, std::memory_order_relaxed);
        }
        void store (const sculptclean::LearnedState&, std::uint64_t learnGeneration,
                    std::uint64_t restoreGeneration) noexcept;
        bool load (sculptclean::LearnedState&, std::uint64_t& version,
                   std::uint64_t* learnGeneration = nullptr,
                   std::uint64_t* restoreGeneration = nullptr) const noexcept;
        std::atomic<std::uint64_t> sequence { 0 };
        std::array<std::atomic<float>, 1025> noisePower {};
        std::atomic<bool> noiseReady { false };
        std::atomic<double> sampleRate { 48000.0 };
        std::atomic<std::uint64_t> consumedLearnGeneration { 0 }, consumedRestoreGeneration { 0 };
    };
    std::array<AtomicLearnedState, 2> publishedLearnedStates {};
    std::atomic<unsigned> completedPublishedSlot { 0 };
    AtomicLearnedState pendingLearnedState;
    std::uint64_t appliedRestoreVersion = 0; // Audio/lifecycle thread only.
    bool lastPublishedNoiseReady = false; // Audio/lifecycle thread only.

    std::mutex stateIoMutex; // XML callbacks only, never held in process().
    sculptclean::LearnedState lastSerializedLearnedState;
    std::uint64_t lastSerializedLearnGeneration = 0, lastSerializedRestoreGeneration = 0;
    sculptclean::CleanEngine engine;

    std::atomic<float>* denoiseParameter = nullptr;
    std::atomic<float>* dereverbParameter = nullptr;
    std::atomic<float>* clickParameter = nullptr;
    std::atomic<float>* humParameter = nullptr;
    std::atomic<float>* humFrequencyParameter = nullptr;
    std::atomic<float>* outputParameter = nullptr;
    std::atomic<float>* bypassParameter = nullptr;
    std::atomic<float>* sourceThresholdParameter = nullptr;
    std::atomic<float>* sourceDepthParameter = nullptr;
    std::atomic<float>* sourceEnabledParameter = nullptr;
    std::atomic<float>* listenRemovedParameter = nullptr;
    std::atomic<std::uint64_t> learnRequestGeneration { 0 };
    std::uint64_t consumedLearnGeneration = 0; // Audio/lifecycle thread only.

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptCleanProcessor)
};

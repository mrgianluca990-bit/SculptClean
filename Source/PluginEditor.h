#pragma once

#include "PluginProcessor.h"
#include <array>
#include <memory>

struct SculptCleanLookAndFeel final : juce::LookAndFeel_V4
{
    SculptCleanLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float position, float startAngle, float endAngle, juce::Slider&) override;
};

class SculptCleanEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SculptCleanEditor (SculptCleanProcessor&);
    ~SculptCleanEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawMeter (juce::Graphics&, juce::Rectangle<float>, float level, const juce::String& label);

    SculptCleanProcessor& processor;
    SculptCleanLookAndFeel look;
    std::array<juce::Slider, 4> amounts;
    juce::Slider output, sourceThreshold, sourceDepth;
    juce::TextButton learn { "Apprendi rumore · 2 s" }, bypass { "BYPASS" };
    juce::TextButton sourceEnabled { "SOURCE OFF" }, listenRemoved { "ASCOLTA RIMOSSO" };
    juce::ComboBox humFrequency;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 4> amountAttachments;
    juce::AudioProcessorValueTreeState::SliderAttachment outputAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment sourceThresholdAttachment, sourceDepthAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment, sourceEnabledAttachment, listenRemovedAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> humFrequencyAttachment;
    juce::TooltipWindow tooltip { this, 650 };
    float inputLevel = 0.0f, outputLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SculptCleanEditor)
};

#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace
{
const juce::Colour cream { 0xfff2ead2 }, warm { 0xffe8935a }, amber { 0xfff0c060 };
const juce::Colour cool { 0xff5fc8c0 }, blue { 0xff84b9e6 }, sourceColour { 0xffb9a0e8 };
const std::array<juce::Colour, 4> accents { warm, amber, cool, blue };
constexpr float startAngle = juce::MathConstants<float>::pi * 1.25f;
constexpr float endAngle = juce::MathConstants<float>::pi * 2.75f;
}

SculptCleanLookAndFeel::SculptCleanLookAndFeel()
{
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff292929));
    setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff474236));
    setColour (juce::TextButton::textColourOffId, cream);
    setColour (juce::TextButton::textColourOnId, cream);
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff292929));
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff444444));
    setColour (juce::ComboBox::textColourId, cream);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff282828));
    setColour (juce::PopupMenu::textColourId, cream);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff282828));
    setColour (juce::TooltipWindow::textColourId, cream);
    setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff555555));
}

void SculptCleanLookAndFeel::drawRotarySlider (juce::Graphics& graphics, int x, int y,
    int width, int height, float position, float a0, float a1, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                        static_cast<float> (width), static_cast<float> (height)).reduced (5.0f);
    const auto centre = bounds.getCentre();
    const float radius = std::min (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const float knobRadius = radius * 0.80f;
    const auto colour = slider.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, a1, true);
    graphics.setColour (colour.withAlpha (0.25f));
    graphics.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (position > 0.0f)
    {
        juce::Path active;
        active.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, a0 + position * (a1 - a0), true);
        graphics.setColour (colour);
        graphics.strokePath (active, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    graphics.setColour (juce::Colour (0xff151515));
    graphics.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);
    graphics.setColour (juce::Colour (0xff373737));
    graphics.drawEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f, 1.5f);

    juce::Path tick;
    tick.addRoundedRectangle (-1.5f, -knobRadius * 0.90f, 3.0f, knobRadius * 0.23f, 1.0f);
    graphics.setColour (cream);
    graphics.fillPath (tick, juce::AffineTransform::rotation (a0 + position * (a1 - a0)).translated (centre.x, centre.y));

    const bool isOutput = slider.getName() == "Output";
    const bool isDecibels = isOutput || slider.getName() == "Source Threshold";
    const auto text = isDecibels ? juce::String (slider.getValue(), 1)
                                : juce::String (static_cast<int> (std::lround (slider.getValue())));
    graphics.setFont (juce::Font (juce::FontOptions (isOutput ? 13.0f : (width < 100 ? 18.0f : 25.0f))));
    graphics.setColour (cream);
    graphics.drawText (text, bounds, juce::Justification::centred);
}

SculptCleanEditor::SculptCleanEditor (SculptCleanProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      outputAttachment (p.apvts, "output", output),
      sourceThresholdAttachment (p.apvts, "sourceThreshold", sourceThreshold),
      sourceDepthAttachment (p.apvts, "sourceDepth", sourceDepth),
      bypassAttachment (p.apvts, "bypass", bypass),
      sourceEnabledAttachment (p.apvts, "sourceEnabled", sourceEnabled),
      listenRemovedAttachment (p.apvts, "listenRemoved", listenRemoved)
{
    setLookAndFeel (&look);
    const std::array<const char*, 4> ids { "denoise", "dereverb", "click", "hum" };
    const std::array<const char*, 4> names { "Denoise", "Dereverb", "De-click", "De-hum" };
    const std::array<const char*, 4> hints {
        "Riduzione spettrale del rumore. Prima apprendi 2 secondi di solo rumore, senza voce o musica. Il profilo resta fisso. Doppio clic: 0%.",
        "Attenua le code del riverbero preservando gli attacchi. Aumenta gradualmente e controlla il segnale rimosso; non ricostruisce una registrazione asciutta. Doppio clic: 0%.",
        "Ripara brevi impulsi isolati. Aumenta gradualmente sui click elettrici; non elimina automaticamente ogni rumore della bocca. Doppio clic: 0%.",
        "Attenua il ronzio di rete e le sue armoniche. Scegli 50 Hz o 60 Hz in base alla registrazione. Doppio clic: 0%."
    };
    for (std::size_t i = 0; i < amounts.size(); ++i)
    {
        auto& slider = amounts[i];
        slider.setName (names[i]);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (startAngle, endAngle, true);
        slider.setColour (juce::Slider::rotarySliderFillColourId, accents[i]);
        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setTextValueSuffix (" %");
        slider.setPopupDisplayEnabled (true, false, this);
        slider.setTooltip (hints[i]);
        addAndMakeVisible (slider);
        amountAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, ids[i], slider);
    }
    output.setName ("Output");
    output.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    output.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    output.setRotaryParameters (startAngle, endAngle, true);
    output.setColour (juce::Slider::rotarySliderFillColourId, cream);
    output.setDoubleClickReturnValue (true, 0.0);
    output.setTextValueSuffix (" dB");
    output.setTooltip ("Volume di uscita. Doppio clic: 0 dB.");
    output.setPopupDisplayEnabled (true, false, this);
    addAndMakeVisible (output);

    sourceThreshold.setName ("Source Threshold");
    sourceDepth.setName ("Source Depth");
    for (auto* slider : { &sourceThreshold, &sourceDepth })
    {
        slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider->setRotaryParameters (startAngle, endAngle, true);
        slider->setColour (juce::Slider::rotarySliderFillColourId, sourceColour);
        slider->setPopupDisplayEnabled (true, false, this);
        addAndMakeVisible (*slider);
    }
    sourceThreshold.setTextValueSuffix (" dBFS");
    sourceThreshold.setDoubleClickReturnValue (true, -32.0);
    sourceThreshold.setTooltip (juce::String::fromUTF8 ("Soglia del Source Extractor. Alzala finché i rientri scendono nelle pause. Doppio clic: -32 dBFS."));
    sourceDepth.setTextValueSuffix (" %");
    sourceDepth.setDoubleClickReturnValue (true, 50.0);
    sourceDepth.setTooltip (juce::String::fromUTF8 ("Massima attenuazione nelle pause: 0–48 dB. 50% = 24 dB. Doppio clic: 50%."));
    sourceEnabled.setClickingTogglesState (true);
    sourceEnabled.setTooltip ("Expander derivato da Sculpt Strip: riduce i rientri sotto soglia, con stereo collegato. DSP classico, senza separazione vocale AI.");
    sourceEnabled.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff514167));

    bypass.setClickingTogglesState (true);
    bypass.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff784329));
    bypass.setTooltip ("Ascolta il segnale originale con la stessa latenza del segnale elaborato.");
    listenRemoved.setClickingTogglesState (true);
    listenRemoved.setColour (juce::TextButton::buttonOnColourId, amber);
    listenRemoved.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff191919));
    listenRemoved.setTooltip (juce::String::fromUTF8 ("Ascolta ciò che viene rimosso. Se senti troppo segnale utile, riduci l'intervento. Bypass ha la precedenza."));
    learn.setTooltip ("Premi durante 2 secondi di solo rumore, senza voce o musica. Avvia la riproduzione nella DAW per completare l'apprendimento.");
    learn.onClick = [this] { processor.requestNoiseLearn(); };
    for (auto* button : { &learn, &bypass, &sourceEnabled, &listenRemoved }) addAndMakeVisible (*button);

    humFrequency.setName ("Hum Frequency");
    humFrequency.addItem ("50 Hz", 1);
    humFrequency.addItem ("60 Hz", 2);
    humFrequency.setJustificationType (juce::Justification::centred);
    humFrequency.setTooltip ("Frequenza del ronzio di rete. 50 Hz in Italia e in gran parte d'Europa; 60 Hz per registrazioni con rete a 60 Hz.");
    addAndMakeVisible (humFrequency);
    humFrequencyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, "humFrequency", humFrequency);

    setOpaque (true);
    setSize (960, 600);
    startTimerHz (25);
    timerCallback();
}

SculptCleanEditor::~SculptCleanEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SculptCleanEditor::resized()
{
    for (std::size_t i = 0; i < amounts.size(); ++i)
        amounts[i].setBounds (52 + static_cast<int> (i) * 235, 105, 150, 150);
    learn.setBounds (40, 288, 175, 28);
    humFrequency.setBounds (773, 288, 119, 28);
    bypass.setBounds (838, 15, 102, 25);
    listenRemoved.setBounds (20, 15, 178, 25);
    sourceThreshold.setBounds (324, 397, 84, 84);
    sourceDepth.setBounds (478, 397, 84, 84);
    sourceEnabled.setBounds (692, 386, 205, 28);
    output.setBounds (849, 517, 68, 68);
}

void SculptCleanEditor::timerCallback()
{
    inputLevel = std::max (processor.display.inputPeak.load (std::memory_order_relaxed), inputLevel * 0.83f);
    outputLevel = std::max (processor.display.outputPeak.load (std::memory_order_relaxed), outputLevel * 0.83f);
    const bool learning = processor.display.noiseLearning.load (std::memory_order_relaxed);
    learn.setEnabled (! learning);
    learn.setButtonText (juce::String::fromUTF8 (learning ? "Apprendimento…" : "Apprendi rumore · 2 s"));
    sourceEnabled.setButtonText (sourceEnabled.getToggleState() ? "SOURCE ON" : "SOURCE OFF");
    repaint();
}

void SculptCleanEditor::drawMeter (juce::Graphics& graphics, juce::Rectangle<float> bounds,
                                  float level, const juce::String& label)
{
    const float db = juce::Decibels::gainToDecibels (std::max (0.0f, level), -72.0f);
    const float position = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    graphics.setColour (cream.withAlpha (0.75f));
    graphics.setFont (juce::Font (juce::FontOptions (11.0f)));
    graphics.drawText (label, bounds.withX (20.0f).withWidth (30.0f).expanded (0.0f, 4.0f), juce::Justification::centredLeft);
    graphics.setColour (juce::Colour (0xff272727));
    graphics.fillRoundedRectangle (bounds, 3.0f);
    graphics.setColour (db > 0.0f ? warm : cool);
    graphics.fillRoundedRectangle (bounds.withWidth (bounds.getWidth() * position), 3.0f);
    graphics.setColour (cream.withAlpha (0.75f));
    graphics.drawText (db <= -60.0f ? juce::String::fromUTF8 ("< −60 dB") : juce::String (db, 1) + " dB",
                       bounds.withX (744.0f).withWidth (67.0f).expanded (0.0f, 4.0f), juce::Justification::centredRight);
}

void SculptCleanEditor::paint (juce::Graphics& graphics)
{
    graphics.fillAll (juce::Colour (0xff0d0d0d));
    graphics.setColour (cream);
    graphics.setFont (juce::Font (juce::FontOptions (16.0f)));
    graphics.drawText ("S C U L P T   C L E A N", 240, 9, 480, 28, juce::Justification::centred);
    graphics.setColour (cream.withAlpha (0.45f));
    graphics.setFont (juce::Font (juce::FontOptions (10.0f)));
    graphics.drawText (juce::String::fromUTF8 ("STUDIO   ·   0.2.0"), 340, 38, 280, 18, juce::Justification::centred);

    const std::array<const char*, 4> labels { "DENOISE", "DEREVERB", "DE-CLICK", "DE-HUM" };
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        const int x = 20 + static_cast<int> (i) * 235;
        graphics.setColour (juce::Colour (0xff191919));
        graphics.fillRoundedRectangle (static_cast<float> (x), 64.0f, 215.0f, 290.0f, 8.0f);
        graphics.setColour (accents[i]);
        graphics.setFont (juce::Font (juce::FontOptions (13.0f)));
        graphics.drawText (labels[i], x, 76, 215, 20, juce::Justification::centred);
    }
    graphics.setFont (juce::Font (juce::FontOptions (11.0f)));
    graphics.setColour (cream.withAlpha (0.7f));
    const float noiseGr = processor.display.denoiseReductionDb.load (std::memory_order_relaxed);
    const float reverbGr = processor.display.dereverbReductionDb.load (std::memory_order_relaxed);
    graphics.drawText ("RIDUZIONE  " + juce::String (noiseGr, 1) + " dB", 30, 257, 195, 20, juce::Justification::centred);
    graphics.drawText ("RIDUZIONE  " + juce::String (reverbGr, 1) + " dB", 265, 257, 195, 20, juce::Justification::centred);
    graphics.drawText ("IMPULSI BREVI", 500, 257, 195, 20, juce::Justification::centred);
    graphics.drawText ("RETE E ARMONICHE", 735, 257, 195, 20, juce::Justification::centred);

    const bool learning = processor.display.noiseLearning.load (std::memory_order_relaxed);
    const bool ready = processor.display.noiseProfileReady.load (std::memory_order_relaxed);
    graphics.setColour (ready ? cool.withAlpha (0.8f) : cream.withAlpha (0.6f));
    graphics.drawText (learning ? "Solo rumore, senza musica" : (ready ? "Profilo pronto" : "Apprendi prima il rumore"),
                       30, 323, 195, 17, juce::Justification::centred);
    if (learning)
    {
        const float progress = juce::jlimit (0.0f, 1.0f, processor.display.noiseLearnProgress.load (std::memory_order_relaxed));
        graphics.setColour (warm);
        graphics.fillRoundedRectangle (40.0f, 345.0f, 175.0f * progress, 2.0f, 1.0f);
    }
    graphics.setColour (cream.withAlpha (0.55f));
    graphics.drawFittedText ("Riduce le code ambientali\npreservando gli attacchi.", 268, 287, 189, 43, juce::Justification::centred, 2);
    graphics.drawFittedText ("Ripara click isolati.\nAumenta gradualmente.", 503, 287, 189, 43, juce::Justification::centred, 2);
    graphics.drawText ("Scegli la frequenza di rete", 735, 323, 195, 17, juce::Justification::centred);

    graphics.setColour (juce::Colour (0xff191919));
    graphics.fillRoundedRectangle (20.0f, 368.0f, 920.0f, 134.0f, 8.0f);
    graphics.setColour (sourceColour);
    graphics.setFont (juce::Font (juce::FontOptions (13.0f)));
    graphics.drawText ("SOURCE EXTRACTOR", 36, 386, 245, 20, juce::Justification::centredLeft);
    graphics.setColour (cream.withAlpha (0.65f));
    graphics.setFont (juce::Font (juce::FontOptions (11.0f)));
    graphics.drawFittedText ("Riduce i rientri quando\nla sorgente si ferma.", 36, 418, 245, 44, juce::Justification::centredLeft, 2);
    graphics.drawText ("THRESHOLD", 304, 379, 124, 18, juce::Justification::centred);
    graphics.drawText ("DEPTH", 458, 379, 124, 18, juce::Justification::centred);
    graphics.drawText ("dBFS", 304, 481, 124, 16, juce::Justification::centred);
    graphics.drawText ("MAX " + juce::String (sourceDepth.getValue() * 0.48, 1) + " dB", 458, 481, 124, 16, juce::Justification::centred);
    const float sourceGr = juce::jlimit (0.0f, 48.0f, processor.display.sourceReductionDb.load (std::memory_order_relaxed));
    graphics.drawText ("RIDUZIONE  " + juce::String (sourceGr, 1) + " dB", 682, 429, 225, 20, juce::Justification::centred);
    graphics.setColour (juce::Colour (0xff292929));
    graphics.fillRoundedRectangle (692.0f, 462.0f, 205.0f, 8.0f, 3.0f);
    graphics.setColour (sourceColour);
    graphics.fillRoundedRectangle (692.0f, 462.0f, 205.0f * sourceGr / 48.0f, 8.0f, 3.0f);

    drawMeter (graphics, { 55.0f, 530.0f, 670.0f, 10.0f }, inputLevel, "IN");
    drawMeter (graphics, { 55.0f, 557.0f, 670.0f, 10.0f }, outputLevel, "OUT");
    graphics.setColour (cream.withAlpha (0.5f));
    graphics.setFont (juce::Font (juce::FontOptions (10.0f)));
    const auto latency = processor.display.latencyMilliseconds.load (std::memory_order_relaxed);
    graphics.drawText ("LATENZA  " + juce::String (latency, 1) + " ms", 20, 581, 245, 16, juce::Justification::centredLeft);
    graphics.drawText ("OUTPUT dB", 828, 583, 110, 14, juce::Justification::centred);
}

#pragma once

#include "SamplerPlugin.h"

//==============================================================================
/**
    "Edit sound": the sampler channel's window (right-click a channel in the rack), like
    FL's channel settings. The waveform on top shows the trim (drag the S and E markers),
    the fades (drag the dots) and where each hit is playing; below are four panels:
    sample (start, end, fades, reverse, cut itself), pitch and level, the envelope
    (with its shape) and the filter.
*/
class SamplerEditor  : public juce::Component,
                       private juce::Timer
{
public:
    explicit SamplerEditor (SamplerPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    SamplerPlugin& plugin;
    juce::OwnedArray<fxui::ParamKnob> knobs;
    std::unique_ptr<fxui::ParamToggle> reverseToggle, envelopeToggle;
    std::unique_ptr<fxui::ParamChoice> filterChoice;
    juce::TextButton cutButton, noteLengthButton;
    juce::Rectangle<int> header, wave, sampleArea, pitchArea, envelopeArea, envelopeGraph, filterArea;

    juce::Image waveImage;           // the waveform with the trim and fades (redrawn when they change)
    juce::String waveKey, envelopeKey;
    std::array<float, SamplerPlugin::maxVoices> shownVoices {};
    bool cutShown = false, noteLengthShown = false;

    enum class Handle { none, start, end, fadeIn, fadeOut };
    Handle dragging = Handle::none;

    fxui::ParamKnob* add (te::AutomatableParameter*, const juce::String& label, juce::Colour, const juce::String& tip);
    juce::Rectangle<float> plotArea() const;
    float xForFraction (float f) const;
    float fractionAtX (float x) const;
    Handle handleAt (juce::Point<float>) const;
    juce::String makeWaveKey() const;
    void renderWave();
    void paintEnvelope (juce::Graphics&);
    void refreshCutButton();
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEditor)
};

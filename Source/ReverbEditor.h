#pragma once

#include "ReverbPlugin.h"

//==============================================================================
/**
    The Reverb's window: a picture of the decay (pre-delay, early reflections, the tail
    falling to -60 dB, how long the bass and the highs last) over five panels - room,
    decay, filters, modulation and mix.
*/
class ReverbEditor  : public juce::Component,
                      private juce::Timer
{
public:
    explicit ReverbEditor (ReverbPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ReverbPlugin& plugin;
    juce::OwnedArray<fxui::ParamKnob> knobs;
    std::unique_ptr<fxui::ParamChoice> sideChoice;
    std::unique_ptr<fxui::ParamToggle> tempoToggle;
    fxui::ParamKnob *predelayKnob = nullptr, *divisionKnob = nullptr;
    juce::Rectangle<int> display, roomArea, decayArea, filterArea, modArea, mixArea;
    juce::String shownKey;

    fxui::ParamKnob* add (te::AutomatableParameter*, const juce::String& label, juce::Colour);
    void timerCallback() override;
    void updatePredelayKnob();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbEditor)
};

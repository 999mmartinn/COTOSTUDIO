#pragma once

#include "DelayPlugin.h"

//==============================================================================
/**
    The Delay's window: an echo map (left side above, right side below: where each
    repeat lands and how loud it is) over six panels - time, feedback, modulation,
    diffusion, distortion and output.
*/
class DelayEditor  : public juce::Component,
                     private juce::Timer
{
public:
    explicit DelayEditor (DelayPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    DelayPlugin& plugin;
    juce::OwnedArray<fxui::ParamKnob> knobs;
    std::unique_ptr<fxui::ParamChoice> modelChoice, filterChoice, distChoice;
    std::unique_ptr<fxui::ParamToggle> syncToggle, pitchToggle;
    fxui::ParamKnob *timeKnob = nullptr, *divisionKnob = nullptr;
    juce::Rectangle<int> display, timeArea, fbArea, modArea, diffArea, distArea, outArea;
    juce::String shownKey;

    fxui::ParamKnob* add (te::AutomatableParameter*, const juce::String& label, juce::Colour, bool bipolar = false);
    void timerCallback() override;
    void updateTimeKnob();
    juce::String stateKey() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayEditor)
};

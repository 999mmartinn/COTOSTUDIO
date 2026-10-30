#pragma once

#include "SidechainPlugin.h"

//==============================================================================
/**
    The Sidechain's window, made to be understood without knowing what a sidechain is:
      - "Listen to": the sound that makes this one duck (usually the kick);
      - a moving picture of the last few seconds: the trigger's hits below, and above them
        how loud this sound is let through (the line dips at every hit);
      - four knobs (amount, sensitivity, attack, release) and three ready-made settings;
      - a line of help.
*/
class SidechainEditor  : public juce::Component,
                         private juce::Timer
{
public:
    explicit SidechainEditor (SidechainPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    SidechainPlugin& plugin;
    juce::OwnedArray<fxui::ParamKnob> knobs;
    juce::TextButton sourceButton;
    juce::OwnedArray<juce::TextButton> presetButtons;
    juce::Rectangle<int> sourceArea, display, knobArea, presetArea, helpArea;

    static constexpr int historySize = 120;   // 4 s at 30 frames a second
    std::array<float, historySize> triggerHistory {}, gainHistory {};
    int historyPos = 0;
    juce::String shownSource;

    void chooseSource();
    void updateSourceButton();
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidechainEditor)
};

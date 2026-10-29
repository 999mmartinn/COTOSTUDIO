#pragma once

#include "LimiterPlugin.h"

//==============================================================================
/**
    The Limiter's window: a scrolling display of the last few seconds (input peaks,
    output peaks, the gain envelope, threshold and ceiling lines) above three panels of
    knobs: compressor, limiter, gate & saturation. Click the display to pause it.
*/
class LimiterEditor  : public juce::Component,
                       private juce::Timer
{
public:
    explicit LimiterEditor (LimiterPlugin&);
    ~LimiterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    LimiterPlugin& plugin;
    juce::OwnedArray<fxui::ParamKnob> knobs;
    juce::Rectangle<int> display, compArea, limArea, gateArea;
    std::vector<lim::Frame> history;   // newest at writePos - 1
    int writePos = 0;
    bool paused = false;
    float shownGr = 0.0f, shownOut = 0.0f;

    fxui::ParamKnob* add (te::AutomatableParameter*, const juce::String& label, juce::Colour, bool bipolar = false);
    void timerCallback() override;
    float yForDb (float db) const;

    static constexpr float topDb = 6.0f, bottomDb = -42.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterEditor)
};

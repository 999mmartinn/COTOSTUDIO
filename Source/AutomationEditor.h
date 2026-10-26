#pragma once

#include "EditorState.h"

/**
    The "Automation" tab: the selected automation clip, big enough to edit.
      - Click empty space: add a point there (snaps to steps; hold Alt for free).
      - Drag a point: move it (time and value).
      - Right-click a point: delete it.
    Changes are shared with everyone like notes; each PC plays the result in SONG mode.
*/
class AutomationEditor  : public juce::Component,
                          private juce::Timer
{
public:
    explicit AutomationEditor (EditorState&);
    ~AutomationEditor() override;

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    EditorState& state;
    juce::Label title, hint;
    juce::TextButton deleteButton;
    juce::ValueTree dragPoint;
    juce::Point<float> hover { -1.0f, -1.0f };

    juce::ValueTree getClip() const;
    juce::Rectangle<float> graphArea() const;
    double clipLength() const;
    float xForBeat (double beat) const;
    double beatAtX (float x) const;
    float yForValue (float v) const;
    float valueAtY (float y) const;
    double snapBeat (double beat, bool free) const;
    juce::ValueTree pointAt (juce::Point<float>) const;
    juce::String valueText (float value01) const;
    void timerCallback() override;
};

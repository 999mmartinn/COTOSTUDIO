#pragma once

#include "EditorState.h"

/**
    Piano roll for the selected channel in the viewed pattern.
      - Left-click empty space: add a note; keep dragging right to make it longer.
      - Drag a note: move it (time and pitch).
      - Drag a note's right edge: change its length.
      - Right-click a note: delete it.
    Notes snap to 1/16 steps.
*/
class PianoRoll  : public juce::Component,
                   private juce::Timer
{
public:
    explicit PianoRoll (EditorState&);
    ~PianoRoll() override;

    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int lowestPitch = 24, highestPitch = 96, rowHeight = 14, keyboardWidth = 48;

private:
    class Grid;
    EditorState& state;
    std::unique_ptr<Grid> grid;
    juce::Viewport viewport;
    juce::Label title;
    int lastPlayhead = -2;
    bool scrolledOnce = false;

    void timerCallback() override;
};

#pragma once

#include "EditorState.h"

/**
    Piano roll for the selected channel in the viewed pattern.
      - Left-click empty space: add a note; keep dragging right to make it longer.
      - Drag a note: move it (time and pitch).
      - Drag a note's right edge: change its length.
      - Right-click a note: delete it.
      - Click the piano keys: play them.
      - Click/drag the ruler on top: set where Play starts (jumps there while playing).
    Notes snap to 1/16 steps. Notes are heard while placed or dragged (local only).
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
    class Ruler;
    EditorState& state;
    std::unique_ptr<Grid> grid;
    std::unique_ptr<Ruler> ruler;
    juce::Viewport viewport;
    juce::Label title;
    juce::String heading;
    juce::Colour headingColour { theme::colours::muted };
    static constexpr int titleHeight = 32;
    int lastPlayhead = -2;
    bool scrolledOnce = false;

    void timerCallback() override;
};

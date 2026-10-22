#pragma once

#include "EditorState.h"

/**
    FL-style playlist: lanes on a timeline where patterns are placed as clips.
      - Click empty space: place the selected pattern (drag to position it).
      - Drag a clip: move it (time and lane), snapping to bar / beat / step.
      - Drag a clip's right edge: shorten or lengthen it.
      - Right-click a clip: delete it.
      - Click a clip: open its pattern in the channel rack and piano roll.
      - Ctrl + mouse wheel: zoom.  Ruler: set where SONG mode starts.
*/
class Playlist  : public juce::Component,
                  private juce::Timer
{
public:
    explicit Playlist (EditorState&);
    ~Playlist() override;

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int laneHeight = 30, laneHeaderWidth = 64, rulerHeight = 18;

private:
    class Lanes;
    class Ruler;
    class LaneHeader;
    class ScrollViewport;

    EditorState& state;
    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<LaneHeader> header;
    std::unique_ptr<ScrollViewport> viewport;
    juce::Label title;
    juce::ComboBox snapBox;
    float pixelsPerBeat = 22.0f;
    bool wasPlaying = false;

    void setZoom (float newPixelsPerBeat, float anchorX);
    void layoutContent();
    void timerCallback() override;
};

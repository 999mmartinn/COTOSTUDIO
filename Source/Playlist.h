#pragma once

#include "EditorState.h"
#include "Bouncer.h"

/**
    FL-style playlist: lanes on a timeline where patterns are placed as clips.
      - Click empty space: place the selected pattern (drag to position it).
      - Drag a clip: move it (time and lane), snapping to bar / beat / step.
      - Drag a clip's right edge: shorten or lengthen it.
      - Right-click a clip: delete it. Right-click its name strip: its menu (pattern: rename,
        colour, make unique; audio take: the insert it plays through, rename).
      - Tracks (lane headers) have a name, a colour and the insert that takes dropped on
        them play through (right-click the header); dropping or moving a take onto such a
        track routes it there.
      - Click a clip: open its pattern in the channel rack and piano roll.
      - Ctrl + mouse wheel: zoom.  Ruler: set where SONG mode starts.
      - Drop audio files (or "Import audio...") to add audio clips; each file gets
        its own audio channel. Clips can be trimmed from both edges.
      - Tools (like FL): Draw (P) places, moves and resizes; Slice (C) cuts clips in two
        (click one, or drag down across several). Right-click a clip's name: "Cut here".
      - Audio clips: drag the small squares at their top corners to fade them in / out;
        Alt + drag the right edge stretches them (time-stretch); double-click one (or
        "Properties..." in its menu) for volume, fades, pitch and speed.
      - Track groups (drag over track names, right-click "Group tracks..."): a header row
        that folds the group away (this PC only) and mutes it (shared). Right-click a
        track's name to mute just that track.
      - Markers: a strip under the bar numbers with shared notes ("drop", "verse 2");
        click one to play from it, drag to move, double-click to edit, right-click for
        colour / delete.
      - Selection (2.0, this PC only): Ctrl + drag on empty space draws a box, Ctrl + click
        adds / removes a clip, Ctrl+A takes all. Drag one selected clip to move them all
        (Shift: leave copies behind). Ctrl+C / Ctrl+X, Ctrl+V (at the mouse, else at the
        song position), Ctrl+B (a copy right after), Delete; a click on empty space lets go.
*/
class Playlist  : public juce::Component,
                  private juce::Timer
{
public:
    explicit Playlist (EditorState&);
    ~Playlist() override;

    void refresh();
    /** Asks for audio files and imports them at the song start marker. */
    void showImportDialog();
    /** Imports audio files in the background and places them from a lane/beat on. */
    void importFiles (const juce::Array<juce::File>& files, int lane, double startBeat);
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    static constexpr int headerHeight = 36;

    // The ruler: bar numbers, tempo tags and punch range on top, markers in a strip below.
    static constexpr int laneHeight = 30, laneHeaderWidth = 118, rulerBarsHeight = 20, rulerHeight = 34;

    /** Full screen: the playlist fills the main window (set up by the main window). */
    std::function<void()> onFullScreenToggle;
    /** The punch in / out range was changed on the ruler (Ctrl + drag, right-click). */
    std::function<void()> onPunchChanged;
    void setFullScreenState (bool isFullScreen);

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
    juce::TextButton importButton { TRANS ("Import audio...") }, automateButton { TRANS ("Automate...") }, fullScreenButton;
    juce::TextButton drawToolButton, sliceToolButton;
    bool sliceTool = false;
    void setSliceTool (bool);
    /** The settings panel of an audio clip (volume, fades, pitch, speed), next to it. */
    void showClipProperties (const juce::String& clipSid, juce::Rectangle<int> clipArea);
    /** Replaces a clip with its sound as audio clips (see Bouncer). */
    void bounceClip (const juce::String& clipSid);
    std::unique_ptr<Bouncer> bouncer;
    void showAutomateMenu();
    std::unique_ptr<juce::FileChooser> chooser;
    juce::ThreadPool importPool { 1 };

    // Waveforms of audio files, shared by every clip that uses the file.
    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache thumbCache { 32 };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails;
    juce::AudioThumbnail* getThumbnail (const juce::String& hash);
    float pixelsPerBeat = 22.0f;
    juce::String armSignature;   // the tracks' arm / monitor state last painted (repaints when it changes)
    int ticksSinceArmCheck = 0;
    static constexpr float minPixelsPerBeat = 1.0f;   // 4 pixels a bar in 4/4: 300 bars in 1200 pixels
    bool wasPlaying = false;
    int lastPlayheadX = -1;   // where the playhead was drawn (only its strip is repainted while playing)

    // Rows, top to bottom: tracks, and a header row above each group of tracks (a folded
    // group shows only its header, with a small picture of its clips).
    struct Row
    {
        int lane = -1;               // the track, or -1 for a group's header row
        song::LaneGroup group;       // the group this row belongs to (if any)
        bool folded = false;         // a folded group's header
    };
    std::vector<Row> rows;
    std::vector<int> rowOfLane;      // index = track; a hidden track (folded) maps to its group's header
    void updateRows();
    int rowAtY (int y) const                  { return juce::jlimit (0, (int) rows.size() - 1, y / laneHeight); }
    /** The track at a height: a group's header row counts as its first track. */
    int laneAtRowY (int y) const;
    int laneTop (int lane) const              { return rowOfLane[(size_t) juce::jlimit (0, song::numLanes - 1, lane)] * laneHeight; }
    bool isLaneHidden (int lane) const;
    int totalRowsHeight() const               { return (int) rows.size() * laneHeight; }
    /** Folds or unfolds a group (this PC only). */
    void setGroupFolded (const juce::String& groupSid, bool folded);

    void setZoom (float newPixelsPerBeat, float anchorX);
    void layoutContent();
    void timerCallback() override;
};

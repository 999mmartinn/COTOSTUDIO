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

    static constexpr int laneHeight = 30, laneHeaderWidth = 78, rulerHeight = 20;

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
    bool wasPlaying = false;

    void setZoom (float newPixelsPerBeat, float anchorX);
    void layoutContent();
    void timerCallback() override;
};

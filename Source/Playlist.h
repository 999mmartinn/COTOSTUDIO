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
      - Drop audio files (or "Import audio...") to add audio clips; each file gets
        its own audio channel. Audio clips can be trimmed from both edges.
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

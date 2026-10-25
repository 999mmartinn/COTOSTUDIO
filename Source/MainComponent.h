#pragma once

#include "EditorState.h"
#include "ChannelRack.h"
#include "PianoRoll.h"
#include "Playlist.h"
#include "Mixer.h"
#include "Browser.h"
#include "Look.h"

//==============================================================================
class MainComponent  : public juce::Component,
                       public juce::DragAndDropContainer,
                       private juce::Timer,
                       private juce::ValueTree::Listener,
                       private juce::AsyncUpdater
{
public:
    explicit MainComponent (te::Engine&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** The window's close button: asks to save changes first. */
    void requestQuit();

private:
    te::Engine& engine;
    struct SimulatedMic;                          // developer option --simulate-mic
    std::unique_ptr<SimulatedMic> simulatedMic;   // declared first: outlives the session
    collab::Session session { engine };
    EditorState state { session };
    std::unique_ptr<juce::InterProcessLock> assetFolderLock;   // which copy on this PC we are
    juce::ApplicationProperties appProperties;
    juce::TooltipWindow tooltipWindow { this, 700 };   // shows the help text on buttons and knobs
    juce::ValueTree watchedState;

    // Song files (File menu)
    MenuButton fileButton { "File" };
    juce::File currentFile;                    // where Save writes ("" = never saved)
    juce::int64 savedHash = 0, lastAutosaveHash = 0;
    bool songChanged = false, dirty = false, expectingOwnSong = false;
    int ticksSinceHashCheck = 0, ticksSinceAutosave = 0;
    juce::RecentlyOpenedFilesList recentFiles;
    std::unique_ptr<juce::FileChooser> fileChooser;

    void showFileMenu();
    void newSong();
    void openSong();
    void openSongFile (const juce::File&);
    void saveSong (bool saveAs, std::function<void (bool saved)> then = {});
    void writeSong (const juce::File&, std::function<void (bool saved)> then);
    void askToSaveChanges (std::function<void()> proceed);
    void autosave();
    void updateTitle();
    juce::int64 currentHash() const;
    static juce::File songsFolder();
    static juce::File autosaveFolder();

    // Connection bar
    juce::TextButton hostButton { "Host" }, joinButton { "Join" }, leaveButton { "Leave" };
    juce::TextEditor portEditor, addressEditor;
    juce::Label portLabel { {}, "Port" }, addressLabel { {}, "Host address" }, statusLabel;

    // Transport / pattern bar
    juce::TextButton patModeButton { "PAT" }, songModeButton { "SONG" };
    juce::TextButton recordButton;
    bool lastRecording = false;
    void showRecordSetupMenu();
    void timerCallback() override;
    juce::TextButton playButton { "Play" }, newPatternButton { "+" }, patternMenuButton { "..." },
                     undoButton { "Undo" }, redoButton { "Redo" },
                     scanButton { "Scan plugins..." }, audioButton { "Audio settings..." };
    juce::Slider tempoSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label tempoLabel { {}, "BPM" }, patternLabel { {}, "Pattern" }, playingLabel;
    juce::ComboBox patternBox, lengthBox;

    Browser browser { state };
    juce::TextButton showBrowserButton { ">" };
    bool browserVisible = true;
    void setBrowserVisible (bool);
    Playlist playlist { state };
    ChannelRack rack { engine, state };
    PianoRoll pianoRoll { state };
    Mixer mixer { engine, state };
    juce::TabbedComponent bottomTabs { juce::TabbedButtonBar::TabsAtTop };   // Piano roll (F7) / Mixer (F9)
    juce::TextEditor logView;

    // Playlist / rack / piano roll, with draggable dividers between them.
    juce::StretchableLayoutManager panelLayout;
    juce::StretchableLayoutResizerBar divider1 { &panelLayout, 1, false }, divider2 { &panelLayout, 3, false };

    void appendLog (const juce::String&);
    void watchEdit();
    void unwatchEdit();
    void refresh();
    void refreshPatternControls();
    void selectPattern (const juce::String& sid);
    void showPatternMenu();
    void showPluginScanner();
    void showAudioSettings();
    void undo (bool redo);

    void handleAsyncUpdate() override                          { refresh(); }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { songChanged = true; triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { songChanged = true; triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { songChanged = true; triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { songChanged = true; triggerAsyncUpdate(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

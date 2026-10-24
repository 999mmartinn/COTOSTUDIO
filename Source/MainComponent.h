#pragma once

#include "EditorState.h"
#include "ChannelRack.h"
#include "PianoRoll.h"
#include "Playlist.h"
#include "Mixer.h"

//==============================================================================
class MainComponent  : public juce::Component,
                       private juce::ValueTree::Listener,
                       private juce::AsyncUpdater
{
public:
    explicit MainComponent (te::Engine&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    te::Engine& engine;
    collab::Session session { engine };
    EditorState state { session };
    std::unique_ptr<juce::InterProcessLock> assetFolderLock;   // which copy on this PC we are
    juce::ApplicationProperties appProperties;
    juce::ValueTree watchedState;

    // Connection bar
    juce::TextButton hostButton { "Host" }, joinButton { "Join" }, leaveButton { "Leave" };
    juce::TextEditor portEditor, addressEditor;
    juce::Label portLabel { {}, "Port" }, addressLabel { {}, "Host address" }, statusLabel;

    // Transport / pattern bar
    juce::TextButton patModeButton { "PAT" }, songModeButton { "SONG" };
    juce::TextButton playButton { "Play" }, newPatternButton { "+" }, patternMenuButton { "..." },
                     undoButton { "Undo" }, redoButton { "Redo" },
                     scanButton { "Scan plugins..." }, audioButton { "Audio settings..." };
    juce::Slider tempoSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label tempoLabel { {}, "BPM" }, patternLabel { {}, "Pattern" }, playingLabel;
    juce::ComboBox patternBox, lengthBox;

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
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

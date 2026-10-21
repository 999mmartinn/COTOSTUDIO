#pragma once

#include "EditorState.h"
#include "ChannelRack.h"
#include "PianoRoll.h"

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
    juce::ApplicationProperties appProperties;
    juce::ValueTree watchedState;

    // Connection bar
    juce::TextButton hostButton { "Host" }, joinButton { "Join" }, leaveButton { "Leave" };
    juce::TextEditor portEditor, addressEditor;
    juce::Label portLabel { {}, "Port" }, addressLabel { {}, "Host address" }, statusLabel;

    // Transport / pattern bar
    juce::TextButton playButton { "Play" }, newPatternButton { "+" }, patternMenuButton { "..." },
                     undoButton { "Undo" }, redoButton { "Redo" },
                     scanButton { "Scan plugins..." }, audioButton { "Audio settings..." };
    juce::Slider tempoSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label tempoLabel { {}, "BPM" }, patternLabel { {}, "Pattern" }, playingLabel;
    juce::ComboBox patternBox, lengthBox;

    ChannelRack rack { engine, state };
    PianoRoll pianoRoll { state };
    juce::TextEditor logView;

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

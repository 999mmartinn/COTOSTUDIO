#pragma once

#include "Session.h"

//==============================================================================
/** 16-step x 12-note grid editing the selected track's pattern. */
class StepGrid  : public juce::Component
{
public:
    std::function<te::Edit*()> getEdit;
    int trackIndex = 0;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int lowestPitch = 60, numRows = 12;

private:
    juce::Rectangle<float> cellBounds (int step, int row) const;
};

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

private:
    te::Engine& engine;
    collab::Session session { engine };
    juce::ApplicationProperties appProperties;
    juce::ValueTree watchedState;
    int selectedTrack = 0;

    juce::TextButton hostButton { "Host" }, joinButton { "Join" }, leaveButton { "Leave" };
    juce::TextEditor portEditor, addressEditor;
    juce::Label portLabel { {}, "Port" }, addressLabel { {}, "Host address" }, statusLabel;

    juce::TextButton playButton { "Play" }, addTrackButton { "+ Track" }, addPluginButton { "+ Plugin" },
                     scanButton { "Scan plugins..." }, audioButton { "Audio settings..." };

    juce::Component trackPanel;
    juce::OwnedArray<juce::TextButton> trackButtons, pluginButtons;
    StepGrid grid;
    juce::TextEditor logView;

    void appendLog (const juce::String&);
    void watchEdit();
    void unwatchEdit();
    void refresh();
    void rebuildTrackPanel();
    void layoutTrackPanel();
    void showPluginMenu();
    void showPluginScanner();
    void showAudioSettings();
    void showPluginOptions (te::Plugin&);

    void handleAsyncUpdate() override                          { refresh(); }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

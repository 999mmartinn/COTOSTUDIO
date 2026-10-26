#pragma once

#include "EditorState.h"

/**
    FL-style channel rack: one row per channel with mute, volume, pan, name
    and a step sequencer row for the viewed pattern.
*/
class ChannelRack  : public juce::Component,
                     public juce::DragAndDropTarget,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer
{
public:
    ChannelRack (te::Engine&, EditorState&);
    ~ChannelRack() override;

    /** Call when the song changed; rebuilds rows only if the channel list changed. */
    void refresh();

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    /** A sound from your folders: a new sampler channel playing it, or - dropped on a
        sampler channel - that channel's new sound. The file is converted in the
        background, then added to the song (and so sent to the others). */
    void addSoundFile (const juce::File&, const juce::String& ontoChannelID = {});

    /** A MIDI file: its notes replace a channel's notes in the viewed pattern
        ("" = a new Synth channel named after the file). */
    void importMidiFile (const juce::File&, const juce::String& ontoChannelID = {});

    // Sounds dragged from the sound browser ("file:<path>") or from Explorer.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    static constexpr int rowHeight = 34, stepsX = 236, bottomHeight = 38;

    /** Own window (e.g. a second monitor) and full screen there (set up by the main window). */
    std::function<void()> onDetachToggle, onFullScreenToggle;
    /** "Piano roll" in a channel's right-click menu: show that channel's notes, full screen. */
    std::function<void (const juce::String& channelID)> onOpenPianoRoll;
    void setDetached (bool);
    void setFullScreenState (bool isFullScreen);

private:
    class Row;

    te::Engine& engine;
    EditorState& state;
    juce::OwnedArray<Row> rows;
    juce::Component content;
    juce::Viewport viewport;
    juce::TextButton addButton { TRANS ("Channel") };
    juce::ComboBox filterBox;
    juce::TextButton detachButton, fullScreenButton;
    juce::StringArray rowIDs;
    juce::Array<int> rowColourIndexes;
    int lastPlayhead = -2;

    juce::ThreadPool importPool { 1 };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String scrollToChannel;   // a channel just added from a file, scrolled into view once its row exists

    // Where a dragged file would land: a channel (replace its sound / its notes) or a new channel.
    struct DropTarget { juce::File file; juce::String channelID; bool active = false; };
    DropTarget drop;
    DropTarget dropTargetAt (const juce::File&, juce::Point<int>) const;
    void setDrop (DropTarget);
    void dropFile (const juce::File&, juce::Point<int>);
    void soundImported (const juce::File&, const AssetStore::ImportResult&, const juce::String& ontoChannelID);

    void showAddChannelMenu();
    void layoutRows();
    void timerCallback() override;

    friend class Row;
    void showChannelMenu (const juce::String& channelID);
};

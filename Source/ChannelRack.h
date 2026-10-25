#pragma once

#include "EditorState.h"

/**
    FL-style channel rack: one row per channel with mute, volume, pan, name
    and a step sequencer row for the viewed pattern.
*/
class ChannelRack  : public juce::Component,
                     private juce::Timer
{
public:
    ChannelRack (te::Engine&, EditorState&);
    ~ChannelRack() override;

    /** Call when the song changed; rebuilds rows only if the channel list changed. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int rowHeight = 34, stepsX = 230;

private:
    class Row;

    te::Engine& engine;
    EditorState& state;
    juce::OwnedArray<Row> rows;
    juce::Component content;
    juce::Viewport viewport;
    juce::TextButton addButton { "+ Channel" };
    juce::ComboBox filterBox;
    juce::StringArray rowIDs;
    juce::Array<int> rowColourIndexes;
    int lastPlayhead = -2;

    void showAddChannelMenu();
    void layoutRows();
    void timerCallback() override;

    friend class Row;
    void showChannelMenu (const juce::String& channelID);
};

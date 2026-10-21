#pragma once

#include "Session.h"
#include "SongModel.h"

/** What this PC is looking at. Local only: two people can view different patterns. */
struct EditorState
{
    explicit EditorState (collab::Session& s) : session (s) {}

    collab::Session& session;
    juce::String viewedPattern;     // pattern sid
    juce::String selectedChannel;   // channel (track) ID

    std::function<void()> onSelectionChanged;

    te::Edit* getEdit() const                { return session.getEdit(); }

    juce::ValueTree getPattern() const
    {
        auto* edit = getEdit();

        if (edit == nullptr)
            return {};

        auto p = song::findPattern (*edit, viewedPattern);

        if (! p.isValid())
        {
            auto all = song::getPatterns (*edit);
            p = all.isEmpty() ? juce::ValueTree() : all.getFirst();
        }

        return p;
    }

    te::AudioTrack* getSelectedChannel() const
    {
        auto* edit = getEdit();

        if (edit == nullptr)
            return nullptr;

        if (auto* t = song::findChannel (*edit, selectedChannel))
            return t;

        auto channels = song::getChannels (*edit);
        return channels.isEmpty() ? nullptr : channels.getFirst();
    }

    /** Starts a new undo step for a user action. */
    void beginAction() const
    {
        if (auto* edit = getEdit())
            edit->getUndoManager().beginNewTransaction();
    }

    /** The step currently playing in the viewed pattern, or -1. */
    int getPlayheadStep() const
    {
        auto* edit = getEdit();

        if (edit == nullptr || ! edit->getTransport().isPlaying()
             || session.getPlayingPattern() != getPattern()[song::ids::sid].toString())
            return -1;

        auto beat = edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats();
        auto len = song::getLengthBeats (getPattern());
        beat = std::fmod (beat, len);
        return (int) (beat / song::stepBeats);
    }

    static juce::Colour channelColour (int index)
    {
        static const juce::uint32 colours[] = { 0xfff28c28, 0xff5fb8d8, 0xffd8c95f, 0xffd86f8f, 0xff8fd86f, 0xffb88fd8 };
        return juce::Colour (colours[index % 6]);
    }
};

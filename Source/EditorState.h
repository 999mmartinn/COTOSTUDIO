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
    double startBeat = 0.0;         // where Play starts in PAT mode (marker on the piano-roll ruler)
    double songStartBeat = 0.0;     // where Play starts in SONG mode (marker on the playlist ruler)
    double playlistSnapBeats = 4.0; // playlist snap: bar / beat / step, 0 = none
    juce::String placeAudioChannel; // if set, clicking empty playlist space places this audio
                                    // channel's file; otherwise the viewed pattern
    juce::String selectedInsert;    // mixer selection: insert track ID, "" = master

    std::function<void()> onSelectionChanged;

    te::Edit* getEdit() const                { return session.getEdit(); }

    /** The audio channel the playlist places on click, or nullptr (= place the pattern). */
    te::AudioTrack* getPlaceAudioChannel() const
    {
        auto* edit = getEdit();
        auto* t = edit != nullptr && placeAudioChannel.isNotEmpty() ? song::findChannel (*edit, placeAudioChannel) : nullptr;
        return t != nullptr && song::isAudioChannel (*t) ? t : nullptr;
    }

    /** Snaps a beat to the playlist grid (rounding down); Alt held = no snap, like FL. */
    double snapPlaylistBeat (double beat) const
    {
        const double grid = juce::ModifierKeys::currentModifiers.isAltDown() ? 0.0 : playlistSnapBeats;
        beat = juce::jmax (0.0, beat);
        return grid > 0.0 ? std::floor (beat / grid + 1.0e-6) * grid : beat;
    }

    /** Same, rounding to the nearest grid line. */
    double snapPlaylistBeatNearest (double beat) const
    {
        const double grid = juce::ModifierKeys::currentModifiers.isAltDown() ? 0.0 : playlistSnapBeats;
        return snapPlaylistBeat (beat + grid * 0.5);
    }

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

    /** The beat currently playing in the viewed pattern, or -1. */
    double getPlayheadBeat() const
    {
        auto* edit = getEdit();

        if (edit == nullptr || ! edit->getTransport().isPlaying() || session.isSongMode()
             || session.getPlayingPattern() != getPattern()[song::ids::sid].toString())
            return -1.0;

        auto beat = edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats();
        return std::fmod (juce::jmax (0.0, beat), song::getLengthBeats (getPattern()));
    }

    /** The step currently playing in the viewed pattern, or -1. */
    int getPlayheadStep() const
    {
        auto beat = getPlayheadBeat();
        return beat < 0.0 ? -1 : (int) (beat / song::stepBeats);
    }

    /** The beat playing in the song (SONG mode only), or -1. */
    double getSongPlayheadBeat() const
    {
        auto* edit = getEdit();

        if (edit == nullptr || ! edit->getTransport().isPlaying() || ! session.isSongMode())
            return -1.0;

        return juce::jmax (0.0, edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());
    }

    /** Where Play starts for the current mode, in beats. */
    double getPlayStartBeat() const
    {
        if (! session.isSongMode())
            return getStartBeat();

        auto* edit = getEdit();
        const double songLength = edit != nullptr ? song::getSongLengthBeats (*edit) : 4.0;
        return songStartBeat < songLength ? songStartBeat : 0.0;
    }

    /** Play start position, kept inside the viewed pattern. */
    double getStartBeat() const
    {
        return juce::jlimit (0.0, juce::jmax (0.0, song::getLengthBeats (getPattern()) - song::stepBeats), startBeat);
    }

    double beatsToSeconds (double beats) const
    {
        auto* edit = getEdit();
        return edit != nullptr ? edit->tempoSequence.toTime (te::BeatPosition::fromBeats (beats)).inSeconds() : 0.0;
    }

    static juce::Colour channelColour (int index)
    {
        static const juce::uint32 colours[] = { 0xfff28c28, 0xff5fb8d8, 0xffd8c95f, 0xffd86f8f, 0xff8fd86f, 0xffb88fd8 };
        return juce::Colour (colours[index % 6]);
    }
};

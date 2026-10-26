#pragma once

#include "Session.h"
#include "SongModel.h"
#include "Look.h"

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
    enum class RackFilter { all, instruments, audio };
    RackFilter rackFilter = RackFilter::all;   // which channels the rack shows (this PC only)

    std::function<void()> onSelectionChanged;

    // Automation
    juce::String selectedAutomation;          // automation clip sid shown in the Automation tab
    juce::String lastTweaked;                 // "pluginID|paramID" of the last of our own knobs you moved
    /** Makes an automation clip for a knob and opens it (set by the main window). */
    std::function<void (const juce::String& pluginID, const juce::String& paramID)> createAutomation;
    /** An automation clip was clicked: show it (set by the main window). */
    std::function<void()> onAutomationSelected;

    /** Hooks a volume/pan knob up to automation: right-click "Create automation clip",
        and moving it makes it the "last tweaked" control. */
    void enableAutomation (ResetSlider& knob, std::function<te::Plugin*()> getPlugin, const juce::String& paramID)
    {
        knob.onCreateAutomation = [this, getPlugin, paramID]
        {
            if (auto* p = getPlugin())
                if (createAutomation)
                    createAutomation (p->itemID.toString(), paramID);
        };

        auto previous = knob.onDragStart;
        knob.onDragStart = [this, getPlugin, paramID, previous]
        {
            if (previous)
                previous();

            if (auto* p = getPlugin())
                lastTweaked = p->itemID.toString() + "|" + paramID;
        };
    }

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

    static juce::Colour channelColour (int index)      { return theme::channelColour (index); }
};

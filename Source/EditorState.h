#pragma once

#include "Session.h"
#include "SongModel.h"
#include "Look.h"
#include <map>
#include <set>

/** What this PC is looking at. Local only: two people can view different patterns. */
struct EditorState
{
    explicit EditorState (collab::Session& s) : session (s) {}

    collab::Session& session;
    juce::String viewedPattern;     // pattern sid
    juce::String selectedChannel;   // channel (track) ID
    double startBeat = 0.0;         // where Play starts in PAT mode (marker on the piano-roll ruler)
    double songStartBeat = 0.0;     // where Play starts in SONG mode (marker on the playlist ruler)
    static constexpr double snapBar = -1.0;   // a snap setting meaning "a bar" (its length depends on the time signature)
    double playlistSnapBeats = snapBar;       // playlist snap: bar / beat / step, 0 = none
    juce::String placeAudioChannel; // if set, clicking empty playlist space places this audio
                                    // channel's file; otherwise the viewed pattern
    juce::String selectedInsert;    // mixer selection: insert track ID, "" = master
    enum class RackFilter { all, instruments, audio };
    RackFilter rackFilter = RackFilter::all;   // which channels the rack shows (this PC only)

    // The piano roll's scale helper (this PC only): with snapping on, notes land
    // only on notes of this key's scale. Off by default: notes go anywhere.
    bool scaleSnap = false;
    int scaleKey = 0;      // 0 = C ... 11 = B
    int scaleType = 1;     // index in scales::types (1 = minor)

    // Piano roll tool and snap (this PC only), like FL: Pencil (P), Select (E), Slice (C).
    enum class PianoTool { pencil, select, slice };
    PianoTool pianoTool = PianoTool::pencil;
    double pianoSnapBeats = song::stepBeats;   // the grid notes land on, 0 = none (Alt held = none too), snapBar = a bar
    bool ghostNotes = true;                      // the other instruments' notes, faded, behind yours (FL's ghost notes)
    song::NoteProperty laneProperty = song::NoteProperty::velocity;   // what the lane under the notes edits

    /** Beats in a bar of the current song (4 in 4/4). */
    double getBeatsPerBar() const
    {
        auto* edit = getEdit();
        return edit != nullptr ? song::getBeatsPerBar (*edit) : 4.0;
    }

    /** A snap setting in beats (snapBar becomes the bar's length). */
    double resolveSnap (double setting) const          { return setting < 0.0 ? getBeatsPerBar() : setting; }

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
        const double grid = juce::ModifierKeys::currentModifiers.isAltDown() ? 0.0 : resolveSnap (playlistSnapBeats);
        beat = juce::jmax (0.0, beat);
        return grid > 0.0 ? std::floor (beat / grid + 1.0e-6) * grid : beat;
    }

    /** Same, rounding to the nearest grid line. */
    double snapPlaylistBeatNearest (double beat) const
    {
        const double grid = juce::ModifierKeys::currentModifiers.isAltDown() ? 0.0 : resolveSnap (playlistSnapBeats);
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

    // Which instrument you last edited in each pattern (this PC only): clicking the pattern
    // on the playlist opens it in the piano roll on that instrument.
    std::map<juce::String, juce::String> lastChannelOfPattern;   // pattern sid -> channel ID

    /** Notes that the selected channel is the one being edited in the viewed pattern. */
    void rememberChannel()
    {
        if (auto* c = getSelectedChannel(); c != nullptr && ! song::isAudioChannel (*c))
            if (auto p = getPattern(); p.isValid())
                lastChannelOfPattern[p[song::ids::sid].toString()] = song::getChannelID (*c);
    }

    /** The instrument to show for a pattern: the one you last edited in it (if it still
        exists), else the first one with notes in it, else "" (keep the current one). */
    juce::String channelForPattern (const juce::String& patternSid) const
    {
        auto* edit = getEdit();

        if (edit == nullptr)
            return {};

        if (auto it = lastChannelOfPattern.find (patternSid); it != lastChannelOfPattern.end())
            if (auto* c = song::findChannel (*edit, it->second); c != nullptr && ! song::isAudioChannel (*c))
                return it->second;

        auto pattern = song::findPattern (*edit, patternSid);

        if (pattern.isValid())
            for (auto* c : song::getChannels (*edit))
                if (! song::isAudioChannel (*c) && ! song::getNotes (pattern, song::getChannelID (*c)).isEmpty())
                    return song::getChannelID (*c);

        return {};
    }

    /** Shows a pattern in the piano roll on the instrument last edited in it. */
    void viewPattern (const juce::String& patternSid)
    {
        viewedPattern = patternSid;
        placeAudioChannel = {};

        auto channel = channelForPattern (patternSid);

        if (channel.isNotEmpty())
            selectedChannel = channel;
    }

    // Playlist track groups folded on this PC (group sids): folding is personal, like scrolling.
    std::set<juce::String> foldedGroups;

    /** Brings the piano roll to the front (set by the main window). */
    std::function<void()> showPianoRoll;

    /** Starts a new undo step for a user action. */
    void beginAction() const
    {
        if (auto* edit = getEdit())
            edit->getUndoManager().beginNewTransaction();
    }

    /** The beat currently playing in the viewed pattern, or -1.
        PAT mode: the pattern loops. SONG mode: the beat inside whichever playlist clip of
        the viewed pattern is under the song position (a clip plays its pattern once, cut
        at the clip's end, like PatternCompiler), -1 between clips. */
    double getPlayheadBeat() const
    {
        auto* edit = getEdit();

        if (edit == nullptr || ! edit->getTransport().isPlaying())
            return -1.0;

        const auto pattern = getPattern();
        const auto sid = pattern[song::ids::sid].toString();
        const double length = song::getLengthBeats (pattern);
        const double beat = juce::jmax (0.0, edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats());

        if (sid.isEmpty() || length <= 0.0)
            return -1.0;

        if (! session.isSongMode())
            return session.getPlayingPattern() == sid ? std::fmod (beat, length) : -1.0;

        return beatInPattern (*edit, pattern, beat);
    }

    /** SONG mode: the beat inside a pattern at this song beat (the playlist clip of that
        pattern under it), or -1 if none of its clips is there. */
    static double beatInPattern (te::Edit& edit, const juce::ValueTree& pattern, double songBeat)
    {
        const auto sid = pattern[song::ids::sid].toString();
        const double length = song::getLengthBeats (pattern);

        for (auto& c : song::getClips (edit))
        {
            if (sid.isEmpty() || c[song::ids::pattern].toString() != sid)
                continue;

            const double inClip = songBeat - (double) c[song::ids::start];

            if (inClip >= 0.0 && inClip < juce::jmin (song::getClipLength (edit, c), length))
                return inClip;
        }

        return -1.0;
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

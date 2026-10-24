#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

/**
    The FL-style song model, stored inside the Edit's ValueTree so it syncs
    like everything else.

      EDIT
        TRACK channel="1" routeTo=...    <- a channel (instrument + effects + volume/pan)
        TRACK insert="1" routeTo=...     <- a mixer insert (effects + volume/pan); routeTo = insert ID or "" (master)
        SONGMODEL
          PATTERNS
            PATTERN sid name bars colour
              PNOTE sid ch p b l v       <- channel (track ID), pitch, start beat, length beats, velocity
          PLAYLIST
            PCLIP sid pattern lane start length   <- a pattern placed in the song (beats)

    Patterns are the truth. What the engine actually plays is generated from
    them on each PC (see PatternCompiler) and never sent over the network.
*/
namespace song
{
    namespace ids
    {
        #define SONG_DECLARE_ID(name) inline const juce::Identifier name { #name };
        SONG_DECLARE_ID (SONGMODEL)
        SONG_DECLARE_ID (PATTERNS)
        SONG_DECLARE_ID (PATTERN)
        SONG_DECLARE_ID (PNOTE)
        SONG_DECLARE_ID (sid)
        SONG_DECLARE_ID (name)
        SONG_DECLARE_ID (bars)
        SONG_DECLARE_ID (colour)
        SONG_DECLARE_ID (ch)
        SONG_DECLARE_ID (p)
        SONG_DECLARE_ID (b)
        SONG_DECLARE_ID (l)
        SONG_DECLARE_ID (v)
        SONG_DECLARE_ID (channel)
        SONG_DECLARE_ID (rootNote)
        SONG_DECLARE_ID (generated)
        SONG_DECLARE_ID (PLAYLIST)
        SONG_DECLARE_ID (PCLIP)
        SONG_DECLARE_ID (pattern)
        SONG_DECLARE_ID (lane)
        SONG_DECLARE_ID (start)
        SONG_DECLARE_ID (length)
        SONG_DECLARE_ID (insert)
        SONG_DECLARE_ID (routeTo)
        #undef SONG_DECLARE_ID
    }

    constexpr double stepBeats = 0.25;      // a step is a 1/16 note
    constexpr int stepsPerBar = 16;

    juce::String newSyncID();

    //==============================================================================
    // Patterns
    juce::ValueTree getModel (te::Edit&);                       // created if missing
    juce::Array<juce::ValueTree> getPatterns (te::Edit&);
    juce::ValueTree findPattern (te::Edit&, const juce::String& sid);
    juce::ValueTree addPattern (te::Edit&, const juce::String& name, int bars);
    void removePattern (te::Edit&, const juce::ValueTree& pattern);
    juce::String nextPatternName (te::Edit&);
    int getBars (const juce::ValueTree& pattern);
    int getNumSteps (const juce::ValueTree& pattern);
    double getLengthBeats (const juce::ValueTree& pattern);
    void setBars (te::Edit&, juce::ValueTree pattern, int bars);

    //==============================================================================
    // Channels (= audio tracks marked channel="1")
    juce::Array<te::AudioTrack*> getChannels (te::Edit&);
    te::AudioTrack* findChannel (te::Edit&, const juce::String& channelID);
    juce::String getChannelID (const te::AudioTrack&);
    int getRootNote (const te::AudioTrack&);

    enum class InstrumentKind { kick, clap, hat, snare, synth };
    te::AudioTrack* addChannel (te::Edit&, InstrumentKind);
    te::AudioTrack* addChannel (te::Edit&, const juce::PluginDescription& instrument);
    void removeChannel (te::Edit&, te::AudioTrack&);

    /** The channel's instrument: its first plugin. */
    te::Plugin* getInstrument (te::AudioTrack&);
    /** Effects: plugins after the instrument, before volume/meter. */
    juce::Array<te::Plugin*> getEffects (te::AudioTrack&);
    te::Plugin::Ptr addEffect (te::AudioTrack&, const juce::PluginDescription&);
    te::VolumeAndPanPlugin* getVolumePlugin (te::AudioTrack&);

    //==============================================================================
    // Notes
    juce::Array<juce::ValueTree> getNotes (const juce::ValueTree& pattern, const juce::String& channelID);
    bool hasStep (const juce::ValueTree& pattern, const juce::String& channelID, int step);
    void setStep (te::Edit&, juce::ValueTree pattern, const te::AudioTrack& channel, int step, bool on);
    juce::ValueTree addNote (te::Edit&, juce::ValueTree pattern, const juce::String& channelID,
                             int pitch, double beat, double lengthBeats, int velocity = 100);
    void removeNote (te::Edit&, juce::ValueTree note);
    juce::ValueTree findNoteAt (const juce::ValueTree& pattern, const juce::String& channelID, int pitch, double beat);

    //==============================================================================
    // Playlist (song arrangement)
    constexpr int numLanes = 16;

    juce::Array<juce::ValueTree> getClips (te::Edit&);
    juce::ValueTree addClip (te::Edit&, const juce::String& patternSid, int lane, double startBeat);
    void removeClip (te::Edit&, juce::ValueTree clip);
    /** Length in beats: the clip's own length if set, otherwise its pattern's length. */
    double getClipLength (te::Edit&, const juce::ValueTree& clip);
    /** End of the last clip, rounded up to a whole bar (at least one bar). */
    double getSongLengthBeats (te::Edit&);

    //==============================================================================
    // Mixer. Routing is stored as the destination insert's ID ("" = master) in our
    // own property; each PC applies it to the engine locally (see MixerRouter).
    juce::Array<te::AudioTrack*> getInserts (te::Edit&);
    te::AudioTrack* findInsert (te::Edit&, const juce::String& insertID);
    te::AudioTrack* addInsert (te::Edit&, const juce::String& name);
    void removeInsert (te::Edit&, te::AudioTrack&);
    int getInsertNumber (te::Edit&, const te::AudioTrack&);        // 1-based, 0 if not an insert

    juce::String getRoute (const te::AudioTrack&);                 // "" = master
    /** The insert this track really feeds (nullptr = master), ignoring dangling or looping routes. */
    te::AudioTrack* resolveRoute (te::Edit&, const te::AudioTrack&);
    bool canRouteTo (te::Edit&, const te::AudioTrack& source, const juce::String& targetID);
    bool setRoute (te::Edit&, te::AudioTrack& source, const juce::String& targetID);

    /** Effect slots of an insert: every plugin except volume/meter. */
    juce::Array<te::Plugin*> getInsertEffects (te::AudioTrack&);
    te::Plugin::Ptr addInsertEffect (te::AudioTrack&, const juce::PluginDescription&);
    juce::Array<te::Plugin*> getMasterEffects (te::Edit&);
    te::Plugin::Ptr addMasterEffect (te::Edit&, const juce::PluginDescription&);
    /** Built-in effects offered in the slot menu: { xmlTypeName, display name }. */
    juce::Array<std::pair<juce::String, juce::String>> getBuiltInEffects();
    /** Creates a built-in effect (type name) or a third-party one (description). */
    te::Plugin::Ptr createEffect (te::Edit&, const juce::String& builtInType, const juce::PluginDescription* external);
    /** Puts an effect into an insert's chain (or the master's when insert is nullptr)
        at a slot index; -1 or past the end = after the last effect. */
    void putEffect (te::Edit&, te::AudioTrack* insert, te::Plugin::Ptr, int slot);

    /** Moves an effect one slot up (-1) or down (+1) within its chain. */
    void moveEffect (te::Plugin&, int direction);

    //==============================================================================
    /** Fills an empty Edit with the default song: Kick, Clap, Hat, Snare, Synth and a beat. */
    void createDefaultSong (te::Edit&);
}

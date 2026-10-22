#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

/**
    The FL-style song model, stored inside the Edit's ValueTree so it syncs
    like everything else.

      EDIT
        TRACK channel="1" ...            <- a channel (instrument + effects + volume/pan)
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
    /** Fills an empty Edit with the default song: Kick, Clap, Hat, Snare, Synth and a beat. */
    void createDefaultSong (te::Edit&);
}

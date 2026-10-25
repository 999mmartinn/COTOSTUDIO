#pragma once

#include "SongModel.h"

/**
    Turns the song model into what the engine plays, on this PC only.

    Pattern mode: for the pattern being played, song mode: for the whole
    playlist (every placed pattern, shifted to its position), it keeps one
    generated MIDI clip on every channel track (marked generated="1", which the sync layer treats as
    local, so it is never sent). In song mode, audio channels get one generated
    audio clip per audio clip in the playlist, reading this PC's copy of the file. When patterns, notes or channels change it
    updates those clips; if nothing differs it changes nothing.
*/
class PatternCompiler  : private juce::ValueTree::Listener,
                         private juce::AsyncUpdater
{
public:
    PatternCompiler (te::Edit&, AssetStore&);
    ~PatternCompiler() override;

    /** Which pattern plays in pattern mode (empty = the first one). */
    void setPattern (const juce::String& patternSid);
    juce::String getPattern() const;

    /** Song mode plays the playlist; pattern mode loops one pattern. */
    void setSongMode (bool);
    bool isSongMode() const noexcept                    { return songMode; }

    /** Brings the generated clips up to date right away. */
    void compileNow();

    /** The notes the engine will play for a channel, as "pitch@beat+len*vel" strings (for tests). */
    juce::StringArray describeCompiledNotes (te::AudioTrack&) const;

    static te::MidiClip* findGeneratedClip (te::AudioTrack&);

    /** The sound a sampler channel has loaded on this PC ("" = none yet), for tests. */
    static juce::String describeLoadedSample (te::AudioTrack&);

    /** The audio this PC plays for an audio channel, as "asset@start+duration>offset" strings (for tests). */
    juce::StringArray describeCompiledAudio (te::AudioTrack&) const;

    /** Automation this PC plays for a knob, as "seconds=value" strings (for tests). */
    juce::StringArray describeCompiledAutomation (te::AutomatableParameter&) const;

private:
    te::Edit& edit;
    AssetStore& assets;
    juce::String patternSid;
    bool compiling = false, songMode = false;

    void handleAsyncUpdate() override                       { compileNow(); }
    void compileAudio (te::AudioTrack&);
    void compileAutomation();
    void compileSamplers();
    std::map<juce::String, juce::String> compiledCurves;   // "plugin|param" -> what its curve holds now
    void changed (const juce::ValueTree&);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree&) override               { changed (p); }
    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree&, int) override        { changed (p); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override                { changed (p); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternCompiler)
};

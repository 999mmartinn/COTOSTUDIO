#pragma once

#include "SongModel.h"

/**
    Turns the song model into what the engine plays, on this PC only.

    For the pattern being played it keeps one generated MIDI clip on every
    channel track (marked generated="1", which the sync layer treats as
    local, so it is never sent). When patterns, notes or channels change it
    updates those clips; if nothing differs it changes nothing.
*/
class PatternCompiler  : private juce::ValueTree::Listener,
                         private juce::AsyncUpdater
{
public:
    explicit PatternCompiler (te::Edit&);
    ~PatternCompiler() override;

    /** Which pattern plays (empty = the first one). */
    void setPattern (const juce::String& patternSid);
    juce::String getPattern() const;

    /** Brings the generated clips up to date right away. */
    void compileNow();

    /** The notes the engine will play for a channel, as "pitch@beat+len*vel" strings (for tests). */
    juce::StringArray describeCompiledNotes (te::AudioTrack&) const;

    static te::MidiClip* findGeneratedClip (te::AudioTrack&);

private:
    te::Edit& edit;
    juce::String patternSid;
    bool compiling = false;

    void handleAsyncUpdate() override                       { compileNow(); }
    void changed (const juce::ValueTree&);

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier&) override   { changed (v); }
    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree&) override               { changed (p); }
    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree&, int) override        { changed (p); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override                { changed (p); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternCompiler)
};

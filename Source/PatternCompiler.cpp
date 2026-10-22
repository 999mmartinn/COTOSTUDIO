#include "PatternCompiler.h"

PatternCompiler::PatternCompiler (te::Edit& e) : edit (e)
{
    edit.state.addListener (this);
    compileNow();
}

PatternCompiler::~PatternCompiler()
{
    cancelPendingUpdate();
    edit.state.removeListener (this);
}

void PatternCompiler::setPattern (const juce::String& sid)
{
    if (sid != patternSid)
    {
        patternSid = sid;
        compileNow();
    }
}

void PatternCompiler::setSongMode (bool shouldBeSong)
{
    if (shouldBeSong != songMode)
    {
        songMode = shouldBeSong;
        compileNow();
    }
}

juce::String PatternCompiler::getPattern() const
{
    if (song::findPattern (edit, patternSid).isValid())
        return patternSid;

    auto patterns = song::getPatterns (edit);
    return patterns.isEmpty() ? juce::String() : patterns.getFirst()[song::ids::sid].toString();
}

void PatternCompiler::changed (const juce::ValueTree& v)
{
    if (compiling)
        return;

    // Ignore anything happening inside our own generated clips.
    for (auto t = v; t.isValid(); t = t.getParent())
        if ((bool) t[song::ids::generated])
            return;

    triggerAsyncUpdate();
}

te::MidiClip* PatternCompiler::findGeneratedClip (te::AudioTrack& track)
{
    for (auto* c : track.getClips())
        if (auto* mc = dynamic_cast<te::MidiClip*> (c))
            if ((bool) mc->state[song::ids::generated])
                return mc;

    return nullptr;
}

namespace
{
    struct NoteData
    {
        int pitch, velocity;
        double beat, length;

        bool operator< (const NoteData& o) const
        {
            return std::tie (beat, pitch, length, velocity) < std::tie (o.beat, o.pitch, o.length, o.velocity);
        }

        bool operator== (const NoteData& o) const
        {
            return pitch == o.pitch && velocity == o.velocity
                && std::abs (beat - o.beat) < 1.0e-6 && std::abs (length - o.length) < 1.0e-6;
        }
    };
}

void PatternCompiler::compileNow()
{
    cancelPendingUpdate();
    const juce::ScopedValueSetter<bool> svs (compiling, true);

    auto pattern = song::findPattern (edit, getPattern());

    if (! pattern.isValid())
        return;

    const double lengthBeats = songMode ? song::getSongLengthBeats (edit) : song::getLengthBeats (pattern);
    const auto range = edit.tempoSequence.toTime ({ te::BeatPosition(), te::BeatPosition::fromBeats (lengthBeats) });

    // Pattern mode: one "placement" of the pattern at 0. Song mode: every clip in the playlist.
    struct Placement { juce::ValueTree pattern; double start, length; };
    std::vector<Placement> placements;

    if (songMode)
    {
        for (auto& c : song::getClips (edit))
        {
            auto p = song::findPattern (edit, c[song::ids::pattern].toString());

            if (p.isValid())
                placements.push_back ({ p, (double) c[song::ids::start],
                                        juce::jmin (song::getClipLength (edit, c), song::getLengthBeats (p)) });
        }
    }
    else
    {
        placements.push_back ({ pattern, 0.0, lengthBeats });
    }

    for (auto* track : song::getChannels (edit))
    {
        // What the placements say this channel should play (notes cut at each clip's end).
        std::vector<NoteData> wanted;

        for (auto& pl : placements)
        {
            for (auto& n : song::getNotes (pl.pattern, song::getChannelID (*track)))
            {
                const double b = n[song::ids::b];

                if (b < pl.length - 1.0e-6)
                    wanted.push_back ({ (int) n[song::ids::p], (int) n[song::ids::v], pl.start + b,
                                        juce::jmin ((double) n[song::ids::l], pl.length - b) });
            }
        }

        std::sort (wanted.begin(), wanted.end());

        auto* clip = findGeneratedClip (*track);

        if (clip == nullptr)
        {
            auto newClip = track->insertMIDIClip ("Pattern", range, nullptr);

            if (newClip == nullptr)
                continue;

            newClip->state.setProperty (song::ids::generated, true, nullptr);
            clip = newClip.get();
        }

        if (clip->getPosition().time != range)
            clip->setPosition ({ range, te::TimeDuration() });

        // Only rewrite the notes when they actually differ.
        auto& seq = clip->getSequence();
        std::vector<NoteData> current;

        for (auto* n : seq.getNotes())
            current.push_back ({ n->getNoteNumber(), n->getVelocity(), n->getStartBeat().inBeats(), n->getLengthBeats().inBeats() });

        std::sort (current.begin(), current.end());

        if (current != wanted)
        {
            seq.clear (nullptr);

            for (auto& n : wanted)
                seq.addNote (n.pitch, te::BeatPosition::fromBeats (n.beat), te::BeatDuration::fromBeats (n.length),
                             n.velocity, 0, nullptr);
        }
    }

    auto& transport = edit.getTransport();

    if (transport.getLoopRange() != range)
        transport.setLoopRange (range);

    transport.looping = true;
}

juce::StringArray PatternCompiler::describeCompiledNotes (te::AudioTrack& track) const
{
    juce::StringArray out;

    if (auto* clip = findGeneratedClip (track))
        for (auto* n : clip->getSequence().getNotes())
            out.add (juce::String (n->getNoteNumber()) + "@" + juce::String (n->getStartBeat().inBeats(), 3)
                      + "+" + juce::String (n->getLengthBeats().inBeats(), 3) + "*" + juce::String (n->getVelocity()));

    out.sort (false);
    return out;
}

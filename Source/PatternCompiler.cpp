#include "PatternCompiler.h"

PatternCompiler::PatternCompiler (te::Edit& e, AssetStore& a) : edit (e), assets (a)
{
    edit.state.addListener (this);
    compileNow();
}

PatternCompiler::~PatternCompiler()
{
    cancelPendingUpdate();
    edit.state.removeListener (this);

    // Take our automation out of the engine (the knobs keep their last value).
    for (auto& [key, sig] : compiledCurves)
        if (auto* p = song::findParameter (edit, key.upToFirstOccurrenceOf ("|", false, false),
                                           key.fromFirstOccurrenceOf ("|", false, false)))
            p->getCurve().clear (nullptr);
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

    // Ignore anything happening inside our own generated clips, and knob values
    // (automation moves them all the time while playing; they don't change what's compiled).
    for (auto t = v; t.isValid(); t = t.getParent())
        if ((bool) t[song::ids::generated] || t.hasType (te::IDs::PLUGIN))
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

    compileAutomation();

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
        if (song::isAudioChannel (*track))
        {
            compileAudio (*track);
            continue;
        }

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

    // While recording the take just runs on (looping would start a second take).
    transport.looping = ! transport.isRecording();
}

void PatternCompiler::compileAudio (te::AudioTrack& track)
{
    // Which audio clips should play on this channel (song mode only, file present here).
    struct Wanted { juce::String sid, hash; te::TimeRange time; te::TimeDuration offset; };
    std::vector<Wanted> wanted;
    const auto channelID = song::getChannelID (track);

    if (songMode)
    {
        for (auto& c : song::getClips (edit))
        {
            if (! song::isAudioClip (c) || c[song::ids::ch].toString() != channelID)
                continue;

            const auto hash = c[song::ids::asset].toString();

            if (! assets.has (hash))
                continue;   // not arrived yet: added as soon as it does

            const double startSeconds = song::beatsToSeconds (edit, c[song::ids::start]);
            wanted.push_back ({ c[song::ids::sid].toString(), hash,
                                te::TimeRange (te::TimePosition::fromSeconds (startSeconds),
                                               te::TimeDuration::fromSeconds ((double) c[song::ids::duration])),
                                te::TimeDuration::fromSeconds ((double) c[song::ids::offset]) });
        }
    }

    // Existing generated audio clips, by the playlist clip they come from.
    std::map<juce::String, te::WaveAudioClip*> existing;

    for (auto* c : track.getClips())
        if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
            if ((bool) wc->state[song::ids::generated])
                existing[wc->state["srcSid"].toString()] = wc;

    for (auto& w : wanted)
    {
        auto it = existing.find (w.sid);
        const te::ClipPosition position { w.time, w.offset };

        if (it != existing.end() && it->second->state["srcHash"].toString() == w.hash)
        {
            auto* clip = it->second;
            existing.erase (it);

            const auto now = clip->getPosition();

            if (now.time != position.time || std::abs (now.offset.inSeconds() - position.offset.inSeconds()) > 1.0e-6)
                clip->setPosition (position);

            continue;
        }

        auto clip = track.insertWaveClip (song::getAssetName (edit, w.hash), assets.getFile (w.hash), position, false);

        if (clip != nullptr)
        {
            clip->state.setProperty (song::ids::generated, true, nullptr);
            clip->state.setProperty ("srcSid", w.sid, nullptr);
            clip->state.setProperty ("srcHash", w.hash, nullptr);
            clip->setAutoTempo (false);
        }
    }

    // Anything left is no longer wanted.
    for (auto& [sid, clip] : existing)
        clip->removeFromParent();
}

//==============================================================================
void PatternCompiler::compileAutomation()
{
    // What each automated knob should do: SONG mode only (like FL), from every automation clip.
    struct Clip { double start, end; juce::ValueTree tree; };
    std::map<juce::String, std::vector<Clip>> byTarget;

    if (songMode)
        for (auto& c : song::getClips (edit))
            if (song::isAutomationClip (c) && ! song::getAutomationPoints (c).isEmpty())
            {
                const double start = c[song::ids::start];
                byTarget[c[song::ids::plugin].toString() + "|" + c[song::ids::param].toString()]
                    .push_back ({ start, start + song::getClipLength (edit, c), c });
            }

    auto& ts = edit.tempoSequence;
    std::map<juce::String, std::vector<std::pair<double, float>>> wanted;   // seconds, 0..1

    for (auto& [key, clips] : byTarget)
    {
        std::sort (clips.begin(), clips.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
        std::vector<std::pair<double, float>> beats;   // beat, 0..1

        for (size_t i = 0; i < clips.size(); ++i)
        {
            auto& c = clips[i];
            // A later clip takes over where it starts (overlaps: the later one wins).
            const double end = i + 1 < clips.size() ? juce::jmin (c.end, clips[i + 1].start) : c.end;

            if (end <= c.start)
                continue;

            // Between clips the knob holds the value the previous clip ended on.
            if (! beats.empty())
                beats.push_back ({ juce::jmax (beats.back().first, c.start - 1.0e-4), beats.back().second });

            beats.push_back ({ c.start, song::getAutomationValueAt (c.tree, 0.0) });

            for (auto& p : song::getAutomationPoints (c.tree))
            {
                const double b = c.start + (double) p[song::ids::b];

                if (b > c.start && b < end)
                    beats.push_back ({ b, (float) p[song::ids::v] });
            }

            beats.push_back ({ end, song::getAutomationValueAt (c.tree, end - c.start) });
        }

        auto& out = wanted[key];

        for (auto& [b, v] : beats)
            out.push_back ({ ts.toTime (te::BeatPosition::fromBeats (b)).inSeconds(), v });
    }

    // Apply: only knobs whose curve actually changes are touched.
    juce::StringArray keys;

    for (auto& [key, sig] : compiledCurves)
        keys.addIfNotAlreadyThere (key);

    for (auto& [key, points] : wanted)
        keys.addIfNotAlreadyThere (key);

    for (auto& key : keys)
    {
        auto* param = song::findParameter (edit, key.upToFirstOccurrenceOf ("|", false, false),
                                           key.fromFirstOccurrenceOf ("|", false, false));
        auto it = wanted.find (key);
        juce::String signature;

        if (it != wanted.end())
            for (auto& [t, v] : it->second)
                signature << juce::String (t, 6) << "=" << juce::String (v, 6) << ";";

        if (param == nullptr)
        {
            compiledCurves.erase (key);
            continue;
        }

        auto existing = compiledCurves.find (key);

        if ((existing == compiledCurves.end() && signature.isEmpty())
             || (existing != compiledCurves.end() && existing->second == signature))
            continue;

        auto& curve = param->getCurve();

        // Built on this PC only: marked generated so it's never sent, and kept out of
        // everyone's undo history.
        curve.state.setProperty (song::ids::generated, true, nullptr);
        curve.clear (nullptr);

        if (it != wanted.end())
        {
            for (auto& [t, v] : it->second)
                curve.addPoint (te::TimePosition::fromSeconds (t), param->valueRange.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, v)), 0.0f, nullptr);

            compiledCurves[key] = signature;
        }
        else
        {
            compiledCurves.erase (key);
        }
    }
}

juce::StringArray PatternCompiler::describeCompiledAutomation (te::AutomatableParameter& param) const
{
    juce::StringArray out;
    auto& curve = param.getCurve();

    for (int i = 0; i < curve.getNumPoints(); ++i)
        out.add (juce::String (curve.getPointTime (i).inSeconds(), 3) + "="
                  + juce::String (param.valueRange.convertTo0to1 (curve.getPointValue (i)), 3));

    return out;
}

juce::StringArray PatternCompiler::describeCompiledAudio (te::AudioTrack& track) const
{
    juce::StringArray out;

    for (auto* c : track.getClips())
        if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
            if ((bool) wc->state[song::ids::generated])
                out.add (wc->state["srcHash"].toString().substring (0, 8) + "@" + juce::String (wc->getPosition().getStart().inSeconds(), 3)
                          + "+" + juce::String (wc->getPosition().getLength().inSeconds(), 3)
                          + ">" + juce::String (wc->getPosition().offset.inSeconds(), 3));

    out.sort (false);
    return out;
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

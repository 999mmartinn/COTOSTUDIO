#include "PatternCompiler.h"
#include "SamplerPlugin.h"

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
        if ((bool) t[song::ids::generated] || t.hasType (te::IDs::PLUGIN) || t.hasType (song::ids::MARKERS))
            return;   // (markers are only notes on the ruler)

    triggerAsyncUpdate();
}

const juce::Identifier PatternCompiler::notePanID ("cotoPan"), PatternCompiler::noteFineID ("cotoFine");

void PatternCompiler::notePropertiesInto (juce::MidiMessageSequence& seq, const te::MidiList& list, te::MidiClip& clip,
                                          te::MidiList::TimeBase timeBase)
{
    // Only our own instruments know these messages (a VST would take them as aftertouch).
    if (! (bool) clip.state[song::ids::generated])
        return;

    auto* track = clip.getAudioTrack();
    auto* instrument = track != nullptr ? song::getInstrument (*track) : nullptr;

    if (dynamic_cast<SamplerPlugin*> (instrument) == nullptr && instrument != nullptr
         && instrument->getPluginType() != juce::String ("drumsynth"))
        return;

    // Each note with a pan or a fine pitch gets a key-pressure message for its own key at the
    // moment it starts: on MIDI channel 16 = pan, 15 = fine pitch (value 64 = centre / none).
    // The instrument applies it to the note that starts there (whichever arrives first).
    for (auto* n : list.getNotes())
    {
        const int pan = n->state[notePanID], fine = n->state[noteFineID];

        if (pan == 0 && fine == 0)
            continue;

        // The same time the engine gives the note-on (our clips have no groove).
        const double time = timeBase == te::MidiList::TimeBase::beatsRaw ? n->getStartBeat().inBeats()
                          : timeBase == te::MidiList::TimeBase::beats    ? n->getPlaybackBeats (te::MidiNote::startEdge, clip, nullptr).inBeats()
                                                                         : n->getPlaybackTime (te::MidiNote::startEdge, clip, nullptr).inSeconds();
        auto toValue = [] (int v) { return juce::jlimit (0, 127, 64 + juce::roundToInt (v * 63.0 / 100.0)); };

        if (pan != 0)   seq.addEvent (juce::MidiMessage::aftertouchChange (16, n->getNoteNumber(), toValue (pan)), juce::jmax (0.0, time));
        if (fine != 0)  seq.addEvent (juce::MidiMessage::aftertouchChange (15, n->getNoteNumber(), toValue (fine)), juce::jmax (0.0, time));
    }

    seq.updateMatchedPairs();
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
        int release = 64, pan = 0, fine = 0;   // note properties (see song::NoteProperty)

        bool operator< (const NoteData& o) const
        {
            return std::tie (beat, pitch, length, velocity, release, pan, fine)
                 < std::tie (o.beat, o.pitch, o.length, o.velocity, o.release, o.pan, o.fine);
        }

        bool operator== (const NoteData& o) const
        {
            return pitch == o.pitch && velocity == o.velocity && release == o.release && pan == o.pan && fine == o.fine
                && std::abs (beat - o.beat) < 1.0e-6 && std::abs (length - o.length) < 1.0e-6;
        }
    };
}

void PatternCompiler::compileNow()
{
    cancelPendingUpdate();
    const juce::ScopedValueSetter<bool> svs (compiling, true);

    compileAutomation();
    compileSamplers();

    auto pattern = song::findPattern (edit, getPattern());

    if (! pattern.isValid())
        return;

    const double lengthBeats = songMode ? song::getSongLengthBeats (edit) : song::getLengthBeats (pattern);
    const auto range = edit.tempoSequence.toTime ({ te::BeatPosition(), te::BeatPosition::fromBeats (lengthBeats) });

    // Pattern mode: one "placement" of the pattern at 0. Song mode: every clip in the playlist.
    // offset: where in the pattern the placement starts (a clip whose start was cut off).
    struct Placement { juce::ValueTree pattern; double start, length, offset; };
    std::vector<Placement> placements;

    if (songMode)
    {
        const auto audible = song::getAudibleLanes (edit);   // (muted tracks and groups don't play)

        for (auto& c : song::getClips (edit))
        {
            if (! audible[(size_t) juce::jlimit (0, song::numLanes - 1, (int) c[song::ids::lane])])
                continue;

            auto p = song::findPattern (edit, c[song::ids::pattern].toString());

            if (p.isValid())
            {
                const double offset = song::getPatternOffset (c);
                placements.push_back ({ p, (double) c[song::ids::start],
                                        juce::jmin (song::getClipLength (edit, c), song::getLengthBeats (p) - offset),
                                        offset });
            }
        }
    }
    else
    {
        placements.push_back ({ pattern, 0.0, lengthBeats, 0.0 });
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
                const double b = (double) n[song::ids::b] - pl.offset;   // within the placement

                if (b >= -1.0e-6 && b < pl.length - 1.0e-6)
                    wanted.push_back ({ (int) n[song::ids::p], (int) n[song::ids::v], pl.start + juce::jmax (0.0, b),
                                        juce::jmin ((double) n[song::ids::l], pl.length - b),
                                        song::getNoteProperty (n, song::NoteProperty::release),
                                        song::getNoteProperty (n, song::NoteProperty::pan),
                                        song::getNoteProperty (n, song::NoteProperty::fine) });
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
            current.push_back ({ n->getNoteNumber(), n->getVelocity(), n->getStartBeat().inBeats(), n->getLengthBeats().inBeats(),
                                 n->getNoteOffVelocity(), (int) n->state[notePanID], (int) n->state[noteFineID] });

        std::sort (current.begin(), current.end());

        if (current != wanted)
        {
            seq.clear (nullptr);

            for (auto& n : wanted)
                if (auto* added = seq.addNote (n.pitch, te::BeatPosition::fromBeats (n.beat), te::BeatDuration::fromBeats (n.length),
                                               n.velocity, 0, nullptr))
                {
                    // Release velocity is MIDI's note-off velocity; pan and fine pitch ride along on the
                    // engine's note and reach our own instruments (see notePropertiesInto).
                    added->setNoteOffVelocity (n.release, nullptr);

                    if (n.pan != 0)   added->state.setProperty (notePanID, n.pan, nullptr);
                    if (n.fine != 0)  added->state.setProperty (noteFineID, n.fine, nullptr);
                }
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
    struct Wanted
    {
        juce::String sid, hash;
        te::TimeRange time;
        te::TimeDuration offset;          // in the engine's terms: file seconds / speed
        double fadeIn, fadeOut, gainDb, pitch, speed;
        bool keepPitch;
    };

    std::vector<Wanted> wanted;
    const auto channelID = song::getChannelID (track);

    if (songMode)
    {
        const auto audible = song::getAudibleLanes (edit);

        for (auto& c : song::getClips (edit))
        {
            if (! song::isAudioClip (c) || c[song::ids::ch].toString() != channelID
                 || ! audible[(size_t) juce::jlimit (0, song::numLanes - 1, (int) c[song::ids::lane])])
                continue;

            const auto hash = c[song::ids::asset].toString();

            if (! assets.has (hash))
                continue;   // not arrived yet: added as soon as it does

            const double startSeconds = song::beatsToSeconds (edit, c[song::ids::start]);
            const double speed = song::getClipSpeed (c);
            wanted.push_back ({ c[song::ids::sid].toString(), hash,
                                te::TimeRange (te::TimePosition::fromSeconds (startSeconds),
                                               te::TimeDuration::fromSeconds ((double) c[song::ids::duration])),
                                te::TimeDuration::fromSeconds ((double) c[song::ids::offset] / speed),
                                song::getClipFadeIn (c), song::getClipFadeOut (c), song::getClipGainDb (c),
                                song::getClipPitch (c), speed, song::getClipKeepsPitch (c) });
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
        te::WaveAudioClip* clip = nullptr;

        if (it != existing.end() && it->second->state["srcHash"].toString() == w.hash)
        {
            clip = it->second;
            existing.erase (it);
        }
        else
        {
            auto created = track.insertWaveClip (song::getAssetName (edit, w.hash), assets.getFile (w.hash),
                                                 { w.time, w.offset }, false);

            if (created == nullptr)
                continue;

            clip = created.get();
            clip->state.setProperty (song::ids::generated, true, nullptr);
            clip->state.setProperty ("srcSid", w.sid, nullptr);
            clip->state.setProperty ("srcHash", w.hash, nullptr);
            clip->setAutoTempo (false);
            clip->setFadeInType (te::AudioFadeCurve::sCurve);
            clip->setFadeOutType (te::AudioFadeCurve::sCurve);
        }

        // Speed and pitch first (they change how long the file lasts on the timeline).
        // "Resample": a pure speed change (like a record), plus any extra pitch on top;
        // with extra pitch the stretcher does both, so the speed's own pitch is added to it.
        const bool stretched = std::abs (w.speed - 1.0) > 1.0e-6 || std::abs (w.pitch) > 1.0e-6;
        const double semitones = w.keepPitch || std::abs (w.pitch) < 1.0e-6
                                    ? w.pitch : w.pitch + 12.0 * std::log2 (w.speed);
        const auto mode = ! stretched || (! w.keepPitch && std::abs (w.pitch) < 1.0e-6)
                            ? te::TimeStretcher::disabled : te::TimeStretcher::signalsmithDefault;

        // Played straight from the file, stretched as it plays (no waiting for a rendered copy).
        if (stretched && clip->canUseProxy())
            clip->setUsesProxy (false);

        if (clip->getTimeStretchMode() != mode)
            clip->setTimeStretchMode (mode);

        if (std::abs (clip->getPitchChange() - (float) semitones) > 1.0e-4f)
            clip->setPitchChange ((float) semitones);

        if (std::abs (clip->getSpeedRatio() - w.speed) > 1.0e-9)
            clip->setSpeedRatio (w.speed);

        const te::ClipPosition position { w.time, w.offset };
        const auto now = clip->getPosition();

        if (now.time != position.time || std::abs (now.offset.inSeconds() - position.offset.inSeconds()) > 1.0e-6)
            clip->setPosition (position);

        if (std::abs (clip->getGainDB() - (float) w.gainDb) > 1.0e-4f)
            clip->setGainDB ((float) w.gainDb);

        if (std::abs (clip->getFadeIn().inSeconds() - w.fadeIn) > 1.0e-6)
            clip->setFadeIn (te::TimeDuration::fromSeconds (w.fadeIn));

        if (std::abs (clip->getFadeOut().inSeconds() - w.fadeOut) > 1.0e-6)
            clip->setFadeOut (te::TimeDuration::fromSeconds (w.fadeOut));
    }

    // Anything left is no longer wanted.
    for (auto& [sid, clip] : existing)
        clip->removeFromParent();
}

//==============================================================================
juce::String PatternCompiler::describeLoadedSample (te::AudioTrack& track)
{
    auto* sampler = dynamic_cast<SamplerPlugin*> (song::getInstrument (track));
    return sampler != nullptr ? sampler->getLoadedHash() : juce::String();
}

void PatternCompiler::compileSamplers()
{
    // Each sampler channel plays this PC's copy of its file (silent until it arrives).
    for (auto* track : song::getChannels (edit))
        if (auto* sampler = dynamic_cast<SamplerPlugin*> (song::getInstrument (*track)))
        {
            const auto hash = sampler->getAsset();
            sampler->loadSample (hash, assets.has (hash) ? assets.getFile (hash) : juce::File());
        }
}

void PatternCompiler::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id)
{
    // A sampler got a new sound: that's inside a plugin, which changed() ignores.
    if (id == SamplerPlugin::assetID && v.hasType (te::IDs::PLUGIN))
    {
        if (! compiling)
            triggerAsyncUpdate();

        return;
    }

    changed (v);
}

//==============================================================================
void PatternCompiler::compileAutomation()
{
    // What each automated knob should do: SONG mode only (like FL), from every automation clip.
    struct Clip { double start, end; juce::ValueTree tree; };
    std::map<juce::String, std::vector<Clip>> byTarget;

    const auto audible = song::getAudibleLanes (edit);

    if (songMode)
        for (auto& c : song::getClips (edit))
            if (song::isAutomationClip (c) && ! song::getAutomationPoints (c).isEmpty()
                 && audible[(size_t) juce::jlimit (0, song::numLanes - 1, (int) c[song::ids::lane])])
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
            {
                auto text = wc->state["srcHash"].toString().substring (0, 8) + "@" + juce::String (wc->getPosition().getStart().inSeconds(), 3)
                              + "+" + juce::String (wc->getPosition().getLength().inSeconds(), 3)
                              + ">" + juce::String (wc->getPosition().offset.inSeconds(), 3);

                // Clip settings, only when set (so older expectations stay the same).
                if (wc->getFadeIn() > te::TimeDuration())    text << " fi=" << juce::String (wc->getFadeIn().inSeconds(), 3);
                if (wc->getFadeOut() > te::TimeDuration())   text << " fo=" << juce::String (wc->getFadeOut().inSeconds(), 3);
                if (std::abs (wc->getGainDB()) > 1.0e-4f)    text << " g=" << juce::String (wc->getGainDB(), 2);
                if (std::abs (wc->getPitchChange()) > 1.0e-4f) text << " p=" << juce::String (wc->getPitchChange(), 2);
                if (std::abs (wc->getSpeedRatio() - 1.0) > 1.0e-6) text << " s=" << juce::String (wc->getSpeedRatio(), 3);

                out.add (text);
            }

    out.sort (false);
    return out;
}

juce::StringArray PatternCompiler::describeCompiledNotes (te::AudioTrack& track) const
{
    juce::StringArray out;

    if (auto* clip = findGeneratedClip (track))
        for (auto* n : clip->getSequence().getNotes())
        {
            auto text = juce::String (n->getNoteNumber()) + "@" + juce::String (n->getStartBeat().inBeats(), 3)
                          + "+" + juce::String (n->getLengthBeats().inBeats(), 3) + "*" + juce::String (n->getVelocity());

            // Note properties, only when set (so older expectations stay the same).
            if (n->getNoteOffVelocity() != 64)      text << " rv=" << n->getNoteOffVelocity();
            if ((int) n->state[notePanID] != 0)     text << " pan=" << (int) n->state[notePanID];
            if ((int) n->state[noteFineID] != 0)    text << " fine=" << (int) n->state[noteFineID];

            out.add (text);
        }

    out.sort (false);
    return out;
}

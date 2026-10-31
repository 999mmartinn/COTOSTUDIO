#include "SongModel.h"
#include "DrumSynthPlugin.h"
#include "SamplerPlugin.h"
#include "ParametricEQPlugin.h"
#include "LimiterPlugin.h"
#include "SidechainPlugin.h"
#include "MoreFx.h"
#include "DelayPlugin.h"
#include "ReverbPlugin.h"

namespace song
{

static juce::UndoManager* um (te::Edit& edit)   { return &edit.getUndoManager(); }

juce::String newSyncID()
{
    return juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());
}

//==============================================================================
juce::ValueTree getModel (te::Edit& edit)
{
    auto model = edit.state.getChildWithName (ids::SONGMODEL);

    if (! model.isValid())
    {
        model = juce::ValueTree (ids::SONGMODEL);
        model.appendChild (juce::ValueTree (ids::PATTERNS), nullptr);
        edit.state.appendChild (model, nullptr);
    }

    if (! model.getChildWithName (ids::PATTERNS).isValid())
        model.appendChild (juce::ValueTree (ids::PATTERNS), nullptr);

    return model;
}

juce::Array<juce::ValueTree> getPatterns (te::Edit& edit)
{
    juce::Array<juce::ValueTree> result;

    for (auto p : getModel (edit).getChildWithName (ids::PATTERNS))
        if (p.hasType (ids::PATTERN))
            result.add (p);

    return result;
}

juce::ValueTree findPattern (te::Edit& edit, const juce::String& sid)
{
    for (auto& p : getPatterns (edit))
        if (p[ids::sid].toString() == sid)
            return p;

    return {};
}

juce::String nextPatternName (te::Edit& edit)
{
    for (int n = 1;; ++n)
    {
        auto name = "Pattern " + juce::String (n);
        bool used = false;

        for (auto& p : getPatterns (edit))
            used = used || p[ids::name].toString() == name;

        if (! used)
            return name;
    }
}

juce::String getTitle (te::Edit& edit)
{
    const auto t = getModel (edit)[ids::title].toString();
    return t.isNotEmpty() ? t : juce::String ("Untitled");
}

void setTitle (te::Edit& edit, const juce::String& title)
{
    getModel (edit).setProperty (ids::title, title.trim().substring (0, 80), nullptr);
}

juce::ValueTree addPattern (te::Edit& edit, const juce::String& name, int bars)
{
    static const juce::uint32 colours[] = { 0xff9b7bf0, 0xff3fa2db, 0xfff08a3c, 0xffe0559a, 0xff38b37c, 0xffe8b530 };   // the Nocturno palette

    juce::ValueTree p (ids::PATTERN);
    p.setProperty (ids::sid, newSyncID(), nullptr);
    p.setProperty (ids::name, name, nullptr);
    p.setProperty (ids::bars, bars, nullptr);
    p.setProperty (ids::colour, juce::Colour (colours[getPatterns (edit).size() % 6]).toString(), nullptr);
    getModel (edit).getChildWithName (ids::PATTERNS).appendChild (p, um (edit));
    return p;
}

void removePattern (te::Edit& edit, const juce::ValueTree& pattern)
{
    auto parent = pattern.getParent();

    if (! parent.isValid() || getPatterns (edit).size() <= 1)   // always keep one
        return;

    // Its placements in the playlist go too.
    for (auto& c : getClips (edit))
        if (c[ids::pattern].toString() == pattern[ids::sid].toString())
            removeClip (edit, c);

    parent.removeChild (pattern, um (edit));
}

juce::ValueTree clonePattern (te::Edit& edit, const juce::ValueTree& pattern)
{
    auto copy = addPattern (edit, pattern[ids::name].toString() + " copy", getBars (pattern));

    for (auto n : pattern)
        if (n.hasType (ids::PNOTE))
            copyNoteProperties (edit, n, addNote (edit, copy, n[ids::ch].toString(), n[ids::p], n[ids::b], n[ids::l], n[ids::v]));

    return copy;
}

//==============================================================================
static juce::ValueTree getPlaylist (te::Edit& edit, bool createIfMissing)
{
    auto model = getModel (edit);
    auto playlist = model.getChildWithName (ids::PLAYLIST);

    if (! playlist.isValid() && createIfMissing)
    {
        playlist = juce::ValueTree (ids::PLAYLIST);
        model.appendChild (playlist, um (edit));
    }

    return playlist;
}

juce::Array<juce::ValueTree> getClips (te::Edit& edit)
{
    juce::Array<juce::ValueTree> result;

    for (auto c : getPlaylist (edit, false))
        if (c.hasType (ids::PCLIP))
            result.add (c);

    return result;
}

juce::ValueTree addClip (te::Edit& edit, const juce::String& patternSid, int lane, double startBeat)
{
    juce::ValueTree c (ids::PCLIP);
    c.setProperty (ids::sid, newSyncID(), nullptr);
    c.setProperty (ids::pattern, patternSid, nullptr);
    c.setProperty (ids::lane, juce::jlimit (0, numLanes - 1, lane), nullptr);
    c.setProperty (ids::start, juce::jmax (0.0, startBeat), nullptr);
    getPlaylist (edit, true).appendChild (c, um (edit));
    return c;
}

juce::ValueTree copyClip (te::Edit& edit, const juce::ValueTree& clip, int lane, double startBeat)
{
    auto c = clip.createCopy();

    std::function<void (juce::ValueTree&)> renew = [&renew] (juce::ValueTree& v)
    {
        if (v.hasProperty (ids::sid))
            v.setProperty (ids::sid, newSyncID(), nullptr);

        for (auto child : v)
            renew (child);
    };

    renew (c);
    c.setProperty (ids::lane, juce::jlimit (0, numLanes - 1, lane), nullptr);
    c.setProperty (ids::start, juce::jmax (0.0, startBeat), nullptr);
    getPlaylist (edit, true).appendChild (c, um (edit));
    return c;
}

void removeClip (te::Edit& edit, juce::ValueTree clip)
{
    auto parent = clip.getParent();

    if (parent.isValid())
        parent.removeChild (clip, um (edit));
}

double getClipLength (te::Edit& edit, const juce::ValueTree& clip)
{
    if (isAutomationClip (clip))
        return juce::jmax (stepBeats, (double) clip.getProperty (ids::length, 16.0));

    if (isAudioClip (clip))
    {
        const double start = clip[ids::start];
        const double endSeconds = beatsToSeconds (edit, start) + (double) clip[ids::duration];
        return juce::jmax (stepBeats / 4.0, secondsToBeats (edit, endSeconds) - start);
    }

    if (clip.hasProperty (ids::length))
        return juce::jmax (stepBeats / 4.0, (double) clip[ids::length]);

    auto pattern = findPattern (edit, clip[ids::pattern].toString());
    return pattern.isValid() ? juce::jmax (stepBeats / 4.0, getLengthBeats (pattern) - getPatternOffset (clip))
                             : 4.0;
}

double getPatternOffset (const juce::ValueTree& clip)
{
    return juce::jmax (0.0, (double) clip.getProperty (ids::poff, 0.0));
}

//==============================================================================
double getClipFadeIn (const juce::ValueTree& clip)     { return juce::jmax (0.0, (double) clip.getProperty (ids::fi, 0.0)); }
double getClipFadeOut (const juce::ValueTree& clip)    { return juce::jmax (0.0, (double) clip.getProperty (ids::fo, 0.0)); }
double getClipGainDb (const juce::ValueTree& clip)     { return juce::jlimit (minClipGainDb, maxClipGainDb, (double) clip.getProperty (ids::gain, 0.0)); }
double getClipPitch (const juce::ValueTree& clip)      { return juce::jlimit (-maxClipPitch, maxClipPitch, (double) clip.getProperty (ids::pitch, 0.0)); }
double getClipSpeed (const juce::ValueTree& clip)      { return juce::jlimit (minClipSpeed, maxClipSpeed, (double) clip.getProperty (ids::speed, 1.0)); }
bool getClipKeepsPitch (const juce::ValueTree& clip)   { return clip[ids::smode].toString() != "resample"; }

double getMaxClipDuration (te::Edit& edit, const juce::ValueTree& clip)
{
    const double fileSeconds = juce::jmax (0.05, getAssetSeconds (edit, clip[ids::asset].toString()));
    return juce::jmax (0.05, (fileSeconds - (double) clip[ids::offset]) / getClipSpeed (clip));
}

/** Sets a number, or removes it when it is back at its default (keeps old songs and the sync small). */
static void setOrClear (te::Edit& edit, juce::ValueTree& v, const juce::Identifier& id, double value, double defaultValue)
{
    if (std::abs (value - defaultValue) < 1.0e-9)
    {
        if (v.hasProperty (id))
            v.removeProperty (id, um (edit));
    }
    else if (! v.hasProperty (id) || std::abs ((double) v[id] - value) > 1.0e-9)
    {
        v.setProperty (id, value, um (edit));
    }
}

void setClipFades (te::Edit& edit, juce::ValueTree clip, double in, double out)
{
    const double duration = juce::jmax (0.0, (double) clip[ids::duration]);
    in = juce::jlimit (0.0, duration, in);
    out = juce::jlimit (0.0, duration - in, out);
    setOrClear (edit, clip, ids::fi, in, 0.0);
    setOrClear (edit, clip, ids::fo, out, 0.0);
}

void setClipSpeed (te::Edit& edit, juce::ValueTree clip, double speed, bool keepLength)
{
    speed = juce::jlimit (minClipSpeed, maxClipSpeed, speed);
    const double oldSpeed = getClipSpeed (clip);
    const double fileSeconds = juce::jmax (0.05, getAssetSeconds (edit, clip[ids::asset].toString()));
    const double offset = clip[ids::offset];
    double duration = clip[ids::duration];

    if (! keepLength)
        duration = duration * oldSpeed / speed;   // the same part of the file

    duration = juce::jlimit (0.01, juce::jmax (0.01, (fileSeconds - offset) / speed), duration);

    setOrClear (edit, clip, ids::speed, speed, 1.0);

    if (std::abs ((double) clip[ids::duration] - duration) > 1.0e-9)
        clip.setProperty (ids::duration, duration, um (edit));

    setClipFades (edit, clip, getClipFadeIn (clip), getClipFadeOut (clip));
}

juce::ValueTree splitClip (te::Edit& edit, juce::ValueTree clip, double beat)
{
    auto parent = clip.getParent();

    if (! parent.isValid())
        return {};

    const double start = clip[ids::start];
    const double length = getClipLength (edit, clip);
    const double cut = beat - start;

    if (cut <= 1.0e-4 || cut >= length - 1.0e-4)
        return {};

    juce::ValueTree right (ids::PCLIP);
    right.copyPropertiesFrom (clip, nullptr);
    right.setProperty (ids::sid, newSyncID(), nullptr);
    right.setProperty (ids::start, beat, nullptr);

    if (isAudioClip (clip))
    {
        // In seconds: the two parts together play exactly what the clip played.
        const double startSeconds = beatsToSeconds (edit, start);
        const double cutSeconds = beatsToSeconds (edit, beat) - startSeconds;
        const double duration = clip[ids::duration];

        if (cutSeconds <= 1.0e-3 || cutSeconds >= duration - 1.0e-3)
            return {};

        const double fadeIn = getClipFadeIn (clip), fadeOut = getClipFadeOut (clip);
        right.setProperty (ids::offset, (double) clip[ids::offset] + cutSeconds * getClipSpeed (clip), nullptr);
        right.setProperty (ids::duration, duration - cutSeconds, nullptr);
        right.removeProperty (ids::fi, nullptr);
        right.removeProperty (ids::fo, nullptr);

        if (fadeOut > 0.0)
            right.setProperty (ids::fo, juce::jmin (fadeOut, duration - cutSeconds), nullptr);

        parent.addChild (right, parent.indexOf (clip) + 1, um (edit));
        clip.setProperty (ids::duration, cutSeconds, um (edit));
        setClipFades (edit, clip, fadeIn, 0.0);
        return right;
    }

    if (isAutomationClip (clip))
    {
        // Each part keeps its points; both get one exactly at the cut, so the line is the same.
        const float atCut = getAutomationValueAt (clip, cut);
        right.setProperty (ids::length, length - cut, nullptr);
        auto addPoint = [] (juce::ValueTree& c, double b, float v)
        {
            juce::ValueTree p (ids::APOINT);
            p.setProperty (ids::sid, newSyncID(), nullptr);
            p.setProperty (ids::b, b, nullptr);
            p.setProperty (ids::v, v, nullptr);
            c.appendChild (p, nullptr);
        };

        addPoint (right, 0.0, atCut);

        for (auto& p : getAutomationPoints (clip))
            if ((double) p[ids::b] > cut + 1.0e-6 && (double) p[ids::b] <= length + 1.0e-6)
                addPoint (right, (double) p[ids::b] - cut, (float) p[ids::v]);

        parent.addChild (right, parent.indexOf (clip) + 1, um (edit));

        for (auto& p : getAutomationPoints (clip))
            if ((double) p[ids::b] > cut + 1.0e-6)
                clip.removeChild (p, um (edit));

        addAutomationPoint (edit, clip, cut, atCut);
        clip.setProperty (ids::length, cut, um (edit));
        return right;
    }

    // A pattern clip: the right part starts that far into the pattern.
    right.setProperty (ids::length, length - cut, nullptr);
    right.setProperty (ids::poff, getPatternOffset (clip) + cut, nullptr);
    parent.addChild (right, parent.indexOf (clip) + 1, um (edit));
    clip.setProperty (ids::length, cut, um (edit));
    return right;
}

double getSongLengthBeats (te::Edit& edit)
{
    double end = 0.0;

    for (auto& c : getClips (edit))
        end = juce::jmax (end, (double) c[ids::start] + getClipLength (edit, c));

    const double bar = getBeatsPerBar (edit);
    return juce::jmax (bar, std::ceil (end / bar - 1.0e-9) * bar);
}

int getBars (const juce::ValueTree& pattern)               { return juce::jlimit (1, maxBars, (int) pattern.getProperty (ids::bars, 1)); }
int getNumSteps (const juce::ValueTree& pattern)           { return getBars (pattern) * getStepsPerBar (pattern); }
double getLengthBeats (const juce::ValueTree& pattern)     { return getBars (pattern) * getBeatsPerBar (pattern); }

//==============================================================================
juce::Array<TimeSignature> getTimeSignatureChoices()
{
    juce::Array<TimeSignature> choices;

    for (auto [num, den] : { std::pair (4, 4), std::pair (3, 4), std::pair (2, 4), std::pair (5, 4),
                             std::pair (6, 4), std::pair (7, 4), std::pair (6, 8), std::pair (12, 8) })
        choices.add ({ num, den });

    return choices;
}

static TimeSignature readTimeSignature (const juce::ValueTree& model)
{
    TimeSignature ts { (int) model.getProperty (ids::tsNum, 4), (int) model.getProperty (ids::tsDen, 4) };

    for (auto& choice : getTimeSignatureChoices())
        if (choice == ts)
            return ts;

    return {};   // anything else (a damaged file): 4/4
}

TimeSignature getTimeSignature (te::Edit& edit)
{
    return readTimeSignature (edit.state.getChildWithName (ids::SONGMODEL));
}

double getBeatsPerBar (te::Edit& edit)                     { return getTimeSignature (edit).beatsPerBar(); }
int getStepsPerBar (te::Edit& edit)                        { return juce::roundToInt (getBeatsPerBar (edit) / stepBeats); }

double getBeatsPerBar (const juce::ValueTree& anyPart)
{
    const auto root = anyPart.getRoot();
    return readTimeSignature (root.hasType (ids::SONGMODEL) ? root : root.getChildWithName (ids::SONGMODEL)).beatsPerBar();
}

int getStepsPerBar (const juce::ValueTree& anyPart)        { return juce::roundToInt (getBeatsPerBar (anyPart) / stepBeats); }

void setTimeSignature (te::Edit& edit, TimeSignature ts)
{
    if (! getTimeSignatureChoices().contains (ts))
        return;

    auto model = getModel (edit);

    if (ts == TimeSignature())
    {
        model.removeProperty (ids::tsNum, um (edit));
        model.removeProperty (ids::tsDen, um (edit));
    }
    else
    {
        model.setProperty (ids::tsNum, ts.numerator, um (edit));
        model.setProperty (ids::tsDen, ts.denominator, um (edit));
    }

    // The engine counts quarter-note beats: 6/8 is 3 of them a bar (the metronome clicks
    // each beat, louder on each bar, and the count-in lasts whole bars).
    if (auto* sig = edit.tempoSequence.getTimeSig (0))
    {
        sig->state.setProperty (te::IDs::numerator, juce::roundToInt (ts.beatsPerBar()), um (edit));
        sig->state.setProperty (te::IDs::denominator, 4, um (edit));
    }
}

//==============================================================================
juce::Array<TempoChange> getTempoChanges (te::Edit& edit)
{
    juce::Array<TempoChange> result;
    auto& seq = edit.tempoSequence;

    for (int i = 1; i < seq.getNumTempos(); ++i)
        if (auto* t = seq.getTempo (i))
            if (auto* previous = seq.getTempo (i - 1))
                result.add ({ t->getStartBeat().inBeats(), t->getBpm(), std::abs (previous->getCurve() - 1.0f) > 1.0e-3f });

    return result;
}

/** Each tempo's curve says how it gets to the next one: 1 = holds, then jumps; 0 = a straight ramp. */
static void setCurveInto (te::Edit& edit, int index, bool ramp)
{
    if (auto* previous = edit.tempoSequence.getTempo (index - 1))
        previous->state.setProperty (te::IDs::curve, ramp ? 0.0f : 1.0f, um (edit));
}

void addTempoChange (te::Edit& edit, double beat, double bpm, bool ramp)
{
    auto& seq = edit.tempoSequence;
    beat = juce::jmax (stepBeats, beat);

    for (int i = 1; i < seq.getNumTempos(); ++i)
        if (std::abs (seq.getTempo (i)->getStartBeat().inBeats() - beat) < 1.0e-6)
            return setTempoChange (edit, i - 1, beat, bpm, ramp);   // one already there: change it

    seq.insertTempo (te::BeatPosition::fromBeats (beat), juce::jlimit (minTempo, maxTempo, bpm), 1.0f);   // (undoable)

    for (int i = 1; i < seq.getNumTempos(); ++i)
        if (std::abs (seq.getTempo (i)->getStartBeat().inBeats() - beat) < 1.0e-6)
            setCurveInto (edit, i, ramp);

    // The last tempo holds forever.
    if (auto* last = seq.getTempo (seq.getNumTempos() - 1))
        last->state.setProperty (te::IDs::curve, 1.0f, um (edit));
}

void setTempoChange (te::Edit& edit, int index, double beat, double bpm, bool ramp)
{
    auto& seq = edit.tempoSequence;
    auto* t = seq.getTempo (index + 1);

    if (t == nullptr)
        return;

    beat = juce::jmax (stepBeats, beat);

    if (std::abs (t->getStartBeat().inBeats() - beat) > 1.0e-6)
    {
        // Moved: taken out and put back where it now belongs (the tempos stay in order).
        removeTempoChange (edit, index);
        addTempoChange (edit, beat, bpm, ramp);
        return;
    }

    t->state.setProperty (te::IDs::bpm, juce::jlimit (minTempo, maxTempo, bpm), um (edit));
    setCurveInto (edit, index + 1, ramp);
}

void removeTempoChange (te::Edit& edit, int index)
{
    auto& seq = edit.tempoSequence;

    if (index < 0 || index + 1 >= seq.getNumTempos())
        return;

    seq.removeTempo (index + 1, false);

    if (auto* last = seq.getTempo (seq.getNumTempos() - 1))
        last->state.setProperty (te::IDs::curve, 1.0f, um (edit));
}

void setBars (te::Edit& edit, juce::ValueTree pattern, int bars)
{
    pattern.setProperty (ids::bars, juce::jlimit (1, maxBars, bars), um (edit));
}

//==============================================================================
juce::Array<te::AudioTrack*> getChannels (te::Edit& edit)
{
    juce::Array<te::AudioTrack*> result;

    for (auto* t : te::getAudioTracks (edit))
        if ((bool) t->state[ids::channel])
            result.add (t);

    return result;
}

juce::String getChannelID (const te::AudioTrack& t)        { return t.itemID.toString(); }
int getRootNote (const te::AudioTrack& t)                  { return (int) t.state.getProperty (ids::rootNote, 60); }

te::AudioTrack* findChannel (te::Edit& edit, const juce::String& channelID)
{
    for (auto* t : getChannels (edit))
        if (getChannelID (*t) == channelID)
            return t;

    return nullptr;
}

static te::AudioTrack* addEmptyChannel (te::Edit& edit, const juce::String& name)
{
    auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, te::getAudioTracks (edit).getLast()), nullptr);

    if (track == nullptr)
        return nullptr;

    track->state.setProperty (ids::channel, true, um (edit));
    track->setName (name);
    return track.get();
}

te::AudioTrack* addChannel (te::Edit& edit, InstrumentKind kind)
{
    if (kind == InstrumentKind::synth)
    {
        auto* track = addEmptyChannel (edit, "Synth");

        if (track != nullptr)
            if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
                track->pluginList.insertPlugin (synth, 0, nullptr);

        return track;
    }

    const int sound = kind == InstrumentKind::kick ? DrumSynthPlugin::kick
                    : kind == InstrumentKind::clap ? DrumSynthPlugin::clap
                    : kind == InstrumentKind::hat  ? DrumSynthPlugin::hat
                                                   : DrumSynthPlugin::snare;

    auto* track = addEmptyChannel (edit, DrumSynthPlugin::getSoundName (sound));

    if (track != nullptr)
    {
        if (auto p = edit.getPluginCache().createNewPlugin (DrumSynthPlugin::xmlTypeName, {}))
        {
            // Go through the parameter (not the stored value directly): the engine only
            // updates a parameter's live value when it is set this way. Setting the
            // stored value alone left new drums playing the default sound (Kick).
            if (auto* drum = dynamic_cast<DrumSynthPlugin*> (p.get()))
                drum->soundParam->setParameter ((float) sound, juce::sendNotification);

            track->pluginList.insertPlugin (p, 0, nullptr);
        }
    }

    return track;
}

te::AudioTrack* addChannel (te::Edit& edit, const juce::PluginDescription& instrument)
{
    auto* track = addEmptyChannel (edit, instrument.name);

    if (track != nullptr)
        if (auto p = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, instrument))
            track->pluginList.insertPlugin (p, 0, nullptr);

    return track;
}

te::AudioTrack* addChannel (te::Edit& edit, te::Plugin::Ptr instrument, const juce::String& name)
{
    auto* track = addEmptyChannel (edit, name);

    if (track != nullptr && instrument != nullptr)
        track->pluginList.insertPlugin (instrument, 0, nullptr);

    return track;
}

void removeChannel (te::Edit& edit, te::AudioTrack& track)
{
    auto id = getChannelID (track);

    // Remove its notes from every pattern, and its audio clips, first.
    for (auto& pattern : getPatterns (edit))
        for (auto& n : getNotes (pattern, id))
            pattern.removeChild (n, um (edit));

    for (auto& c : getClips (edit))
        if (c[ids::ch].toString() == id)
            removeClip (edit, c);

    edit.deleteTrack (&track);
}

bool isGeneratedPlugin (const te::Plugin* p)
{
    return p != nullptr && (bool) p->state[ids::generated];
}

// Volume/meter and the sends built by MixerRouter: they sit after the effects.
static bool isMixerUtility (te::Plugin* p)
{
    return dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr
        || (isGeneratedPlugin (p) && dynamic_cast<te::AuxSendPlugin*> (p) != nullptr);
}

// Not an effect slot: utilities and anything MixerRouter built (the send return sits first).
static bool isNotAnEffect (te::Plugin* p)
{
    return isMixerUtility (p) || isGeneratedPlugin (p);
}

te::Plugin* getInstrument (te::AudioTrack& track)
{
    if (isAudioChannel (track))
        return nullptr;     // audio channels play their clips, they have no instrument

    for (auto* p : track.pluginList.getPlugins())
        if (! isMixerUtility (p))
            return p;

    return nullptr;
}

juce::Array<te::Plugin*> getEffects (te::AudioTrack& track)
{
    juce::Array<te::Plugin*> result;
    auto* instrument = getInstrument (track);

    for (auto* p : track.pluginList.getPlugins())
        if (p != instrument && ! isNotAnEffect (p))
            result.add (p);

    return result;
}

te::Plugin::Ptr addEffect (te::AudioTrack& track, const juce::PluginDescription& desc)
{
    auto plugin = track.edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc);

    if (plugin == nullptr)
        return {};

    // After the instrument and existing effects, before volume/meter.
    int index = 0;
    auto plugins = track.pluginList.getPlugins();

    while (index < plugins.size() && ! isMixerUtility (plugins[index].get()))
        ++index;

    track.pluginList.insertPlugin (plugin, juce::jmax (1, index), nullptr);
    return plugin;
}

te::VolumeAndPanPlugin* getVolumePlugin (te::AudioTrack& track)
{
    for (auto* p : track.pluginList.getPlugins())
        if (auto* v = dynamic_cast<te::VolumeAndPanPlugin*> (p))
            return v;

    return nullptr;
}

//==============================================================================
juce::Array<juce::ValueTree> getNotes (const juce::ValueTree& pattern, const juce::String& channelID)
{
    juce::Array<juce::ValueTree> result;

    for (auto n : pattern)
        if (n.hasType (ids::PNOTE) && n[ids::ch].toString() == channelID)
            result.add (n);

    return result;
}

static int stepOf (const juce::ValueTree& note)
{
    return juce::roundToInt ((double) note[ids::b] / stepBeats);
}

bool hasStep (const juce::ValueTree& pattern, const juce::String& channelID, int step)
{
    for (auto& n : getNotes (pattern, channelID))
        if (stepOf (n) == step)
            return true;

    return false;
}

juce::ValueTree addNote (te::Edit& edit, juce::ValueTree pattern, const juce::String& channelID,
                         int pitch, double beat, double lengthBeats, int velocity)
{
    juce::ValueTree n (ids::PNOTE);
    n.setProperty (ids::sid, newSyncID(), nullptr);
    n.setProperty (ids::ch, channelID, nullptr);
    n.setProperty (ids::p, juce::jlimit (0, 127, pitch), nullptr);
    n.setProperty (ids::b, juce::jmax (0.0, beat), nullptr);
    n.setProperty (ids::l, juce::jmax (stepBeats / 4.0, lengthBeats), nullptr);
    n.setProperty (ids::v, juce::jlimit (1, 127, velocity), nullptr);
    pattern.appendChild (n, um (edit));
    return n;
}

//==============================================================================
static const juce::Identifier& idFor (NoteProperty p)
{
    switch (p)
    {
        case NoteProperty::release:   return ids::rv;
        case NoteProperty::pan:       return ids::pn;
        case NoteProperty::fine:      return ids::fp;
        case NoteProperty::velocity:  break;
    }

    return ids::v;
}

juce::Range<int> getNotePropertyRange (NoteProperty p)
{
    switch (p)
    {
        case NoteProperty::velocity:  return { 1, 127 };
        case NoteProperty::release:   return { 0, 127 };
        case NoteProperty::pan:       return { -100, 100 };
        case NoteProperty::fine:      return { -100, 100 };
    }

    return {};
}

int getNotePropertyDefault (NoteProperty p)
{
    return p == NoteProperty::velocity ? 100 : p == NoteProperty::release ? 64 : 0;
}

int getNoteProperty (const juce::ValueTree& note, NoteProperty p)
{
    const auto range = getNotePropertyRange (p);
    return juce::jlimit (range.getStart(), range.getEnd(), (int) note.getProperty (idFor (p), getNotePropertyDefault (p)));
}

void setNoteProperty (te::Edit& edit, juce::ValueTree note, NoteProperty p, int value)
{
    const auto range = getNotePropertyRange (p);
    value = juce::jlimit (range.getStart(), range.getEnd(), value);
    const auto& id = idFor (p);

    // Velocity is always written (older versions expect it); the others only when not normal.
    if (p != NoteProperty::velocity && value == getNotePropertyDefault (p))
    {
        if (note.hasProperty (id))
            note.removeProperty (id, um (edit));
    }
    else if (! note.hasProperty (id) || (int) note[id] != value)
    {
        note.setProperty (id, value, um (edit));
    }
}

void copyNoteProperties (te::Edit& edit, const juce::ValueTree& from, juce::ValueTree to)
{
    for (auto p : { NoteProperty::release, NoteProperty::pan, NoteProperty::fine })
        setNoteProperty (edit, to, p, getNoteProperty (from, p));
}

void removeNote (te::Edit& edit, juce::ValueTree note)
{
    auto parent = note.getParent();

    if (parent.isValid())
        parent.removeChild (note, um (edit));
}

int setChannelNotes (te::Edit& edit, juce::ValueTree pattern, const juce::String& channelID,
                     const juce::Array<NoteSpec>& notes)
{
    if (! pattern.isValid())
        return 0;

    for (auto& n : getNotes (pattern, channelID))
        pattern.removeChild (n, um (edit));

    const double bar = getBeatsPerBar (edit);
    const double maxBeats = 8 * bar;
    double end = 0.0;
    int added = 0;

    for (auto& n : notes)
    {
        if (n.beat >= maxBeats - 1.0e-6)
            continue;

        const double length = juce::jmin (n.length, maxBeats - n.beat);
        addNote (edit, pattern, channelID, n.pitch, n.beat, length, n.velocity);
        end = juce::jmax (end, n.beat + length);
        ++added;
    }

    const int bars = juce::jlimit (1, maxBars, (int) std::ceil (end / bar - 1.0e-9));

    if (bars > getBars (pattern))
        setBars (edit, pattern, bars);

    return added;
}

juce::ValueTree findNoteAt (const juce::ValueTree& pattern, const juce::String& channelID, int pitch, double beat)
{
    for (auto& n : getNotes (pattern, channelID))
    {
        const double start = n[ids::b], len = n[ids::l];

        if ((int) n[ids::p] == pitch && beat >= start - 1.0e-6 && beat < start + len - 1.0e-6)
            return n;
    }

    return {};
}

void setStep (te::Edit& edit, juce::ValueTree pattern, const te::AudioTrack& channel, int step, bool on)
{
    const auto id = getChannelID (channel);

    if (on)
    {
        if (! hasStep (pattern, id, step))
            addNote (edit, pattern, id, getRootNote (channel), step * stepBeats, stepBeats);
    }
    else
    {
        for (auto& n : getNotes (pattern, id))
            if (stepOf (n) == step)
                removeNote (edit, n);
    }
}

//==============================================================================
//==============================================================================
juce::Array<te::AudioTrack*> getInserts (te::Edit& edit)
{
    juce::Array<te::AudioTrack*> result;

    for (auto* t : te::getAudioTracks (edit))
        if ((bool) t->state[ids::insert])
            result.add (t);

    return result;
}

te::AudioTrack* findInsert (te::Edit& edit, const juce::String& insertID)
{
    if (insertID.isEmpty())
        return nullptr;

    for (auto* t : getInserts (edit))
        if (t->itemID.toString() == insertID)
            return t;

    return nullptr;
}

int getInsertNumber (te::Edit& edit, const te::AudioTrack& track)
{
    return getInserts (edit).indexOf (const_cast<te::AudioTrack*> (&track)) + 1;
}

te::AudioTrack* addInsert (te::Edit& edit, const juce::String& name)
{
    auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, te::getAudioTracks (edit).getLast()), nullptr);

    if (track == nullptr)
        return nullptr;

    track->state.setProperty (ids::insert, true, um (edit));
    track->setName (name.isNotEmpty() ? name : "Insert " + juce::String (getInserts (edit).size()));
    return track.get();
}

void removeInsert (te::Edit& edit, te::AudioTrack& insert)
{
    const auto id = insert.itemID.toString();

    // Anything routed into it goes to the master instead.
    for (auto* t : te::getAudioTracks (edit))
        if (t->state[ids::routeTo].toString() == id)
            t->state.setProperty (ids::routeTo, juce::String(), um (edit));

    // Its sends, and sends into it, go too.
    for (auto s : getSends (edit))
        if (s[ids::from].toString() == id || s[ids::to].toString() == id)
            removeSend (edit, s);

    edit.deleteTrack (&insert);
}

juce::String getRoute (const te::AudioTrack& track)
{
    return track.state[ids::routeTo].toString();
}

bool canRouteTo (te::Edit& edit, const te::AudioTrack& source, const juce::String& targetID)
{
    if (targetID.isEmpty())
        return true;    // master is always fine

    if (findInsert (edit, targetID) == nullptr)
        return false;

    // If the target's sound already comes back to the source (through routes or
    // sends), routing the source into it would make a loop.
    const auto sourceID = source.itemID.toString();
    return targetID != sourceID && ! feeds (edit, targetID, sourceID);
}

bool feeds (te::Edit& edit, const juce::String& fromID, const juce::String& toID)
{
    if (fromID.isEmpty() || toID.isEmpty())
        return false;

    // Everything the sound of fromID flows into: its route, then its sends, recursively.
    juce::StringArray toVisit { fromID }, visited;

    while (! toVisit.isEmpty())
    {
        const auto id = toVisit[toVisit.size() - 1];
        toVisit.remove (toVisit.size() - 1);

        if (visited.contains (id) || visited.size() > 1000)
            continue;

        visited.add (id);

        if (auto* t = findInsert (edit, id))
        {
            const auto route = getRoute (*t);

            if (route == toID)
                return true;

            if (route.isNotEmpty())
                toVisit.add (route);
        }

        for (auto s : getSendsFrom (edit, id))
        {
            const auto to = s[ids::to].toString();

            if (to == toID)
                return true;

            toVisit.add (to);
        }
    }

    return false;
}

//==============================================================================
// Sidechain
juce::Array<te::AudioTrack*> getSidechainSources (te::Edit& edit, te::Plugin& plugin)
{
    juce::Array<te::AudioTrack*> candidates;
    candidates.addArray (getChannels (edit));
    candidates.addArray (getInserts (edit));

    auto* own = dynamic_cast<te::AudioTrack*> (plugin.getOwnerTrack());

    if (own == nullptr)
        return candidates;   // a master effect: everything comes before it

    // Everything the plugin's track reaches: routes, sends, and tracks with an effect that
    // listens to it (their sound waits for it). Listening to one of those would loop.
    juce::StringArray toVisit { own->itemID.toString() }, reached;

    while (! toVisit.isEmpty())
    {
        const auto id = toVisit[toVisit.size() - 1];
        toVisit.remove (toVisit.size() - 1);

        if (reached.contains (id) || reached.size() > 1000)
            continue;

        reached.add (id);
        te::AudioTrack* track = nullptr;

        for (auto* t : candidates)
            if (t->itemID.toString() == id)
                track = t;

        if (track == nullptr)
            continue;

        if (auto* next = resolveRoute (edit, *track))
            toVisit.add (next->itemID.toString());

        for (auto send : getSendsFrom (edit, id))
            toVisit.add (send[ids::to].toString());

        for (auto* p : edit.getPluginCache().getPlugins())
            if (p != &plugin && p->getSidechainSourceID() == track->itemID)
                if (auto* listener = dynamic_cast<te::AudioTrack*> (p->getOwnerTrack()))
                    toVisit.add (listener->itemID.toString());
    }

    juce::Array<te::AudioTrack*> result;

    for (auto* t : candidates)
        if (! reached.contains (t->itemID.toString()))
            result.add (t);

    return result;
}

bool setSidechainSource (te::Edit& edit, te::Plugin& plugin, te::AudioTrack* source)
{
    if (! plugin.canSidechain())
        return false;

    auto* um = &edit.getUndoManager();

    // Rewire: inputs 1-2 are the plugin's own track, the rest come from the source
    // (its left / right).
    while (plugin.getNumWires() > 0)
        if (auto* w = plugin.getWire (0))
            plugin.breakConnection (w->sourceChannelIndex, w->destChannelIndex);
        else
            break;

    if (source == nullptr)
    {
        plugin.state.removeProperty (te::IDs::sidechainSourceID, um);
        return true;
    }

    if (! getSidechainSources (edit, plugin).contains (source))
        return false;

    plugin.setSidechainSourceID (source->itemID);
    juce::StringArray ins;
    plugin.getChannelNames (&ins, nullptr);

    for (int i = 0; i < juce::jmin (2, ins.size()); ++i)
        plugin.makeConnection (i, i, um);

    for (int i = 2; i < ins.size(); ++i)
        plugin.makeConnection (2 + (i - 2) % 2, i, um);

    // The engine's own compressor only listens to its trigger input when told to.
    if (auto* comp = dynamic_cast<te::CompressorPlugin*> (&plugin))
        comp->useSidechainTrigger = true;

    return true;
}

te::AudioTrack* getSidechainSource (te::Edit& edit, te::Plugin& plugin)
{
    const auto id = plugin.getSidechainSourceID();

    if (! id.isValid())
        return nullptr;

    for (auto* t : getChannels (edit))
        if (t->itemID == id)
            return t;

    for (auto* t : getInserts (edit))
        if (t->itemID == id)
            return t;

    return nullptr;
}

//==============================================================================
// Automation clips
static const juce::String autoKind ("auto");

bool isAutomationClip (const juce::ValueTree& clip)
{
    return clip[ids::kind].toString() == autoKind;
}

juce::ValueTree addAutomationClip (te::Edit& edit, const juce::String& pluginID, const juce::String& paramID,
                                   int lane, double startBeat, double lengthBeats, float startValue01)
{
    juce::ValueTree c (ids::PCLIP);
    c.setProperty (ids::sid, newSyncID(), nullptr);
    c.setProperty (ids::kind, autoKind, nullptr);
    c.setProperty (ids::plugin, pluginID, nullptr);
    c.setProperty (ids::param, paramID, nullptr);
    c.setProperty (ids::lane, juce::jlimit (0, numLanes - 1, lane), nullptr);
    c.setProperty (ids::start, juce::jmax (0.0, startBeat), nullptr);
    c.setProperty (ids::length, juce::jmax (stepBeats, lengthBeats), nullptr);

    // A flat line at the knob's value: two points, one at each end.
    for (double b : { 0.0, lengthBeats })
    {
        juce::ValueTree p (ids::APOINT);
        p.setProperty (ids::sid, newSyncID(), nullptr);
        p.setProperty (ids::b, b, nullptr);
        p.setProperty (ids::v, juce::jlimit (0.0f, 1.0f, startValue01), nullptr);
        c.appendChild (p, nullptr);
    }

    getPlaylist (edit, true).appendChild (c, um (edit));
    return c;
}

juce::Array<juce::ValueTree> getAutomationPoints (const juce::ValueTree& clip)
{
    juce::Array<juce::ValueTree> points;

    for (auto p : clip)
        if (p.hasType (ids::APOINT))
            points.add (p);

    std::stable_sort (points.begin(), points.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                      { return (double) a[ids::b] < (double) b[ids::b]; });
    return points;
}

juce::ValueTree addAutomationPoint (te::Edit& edit, juce::ValueTree clip, double beat, float value01)
{
    juce::ValueTree p (ids::APOINT);
    p.setProperty (ids::sid, newSyncID(), nullptr);
    p.setProperty (ids::b, juce::jmax (0.0, beat), nullptr);
    p.setProperty (ids::v, juce::jlimit (0.0f, 1.0f, value01), nullptr);
    clip.appendChild (p, um (edit));
    return p;
}

void moveAutomationPoint (te::Edit& edit, juce::ValueTree point, double beat, float value01)
{
    if (std::abs ((double) point[ids::b] - beat) > 1.0e-9)
        point.setProperty (ids::b, juce::jmax (0.0, beat), um (edit));

    if (std::abs ((float) point[ids::v] - value01) > 1.0e-6f)
        point.setProperty (ids::v, juce::jlimit (0.0f, 1.0f, value01), um (edit));
}

void removeAutomationPoint (te::Edit& edit, juce::ValueTree point)
{
    auto parent = point.getParent();

    if (parent.isValid())
        parent.removeChild (point, um (edit));
}

float getAutomationValueAt (const juce::ValueTree& clip, double beat)
{
    auto points = getAutomationPoints (clip);

    if (points.isEmpty())
        return 0.0f;

    if (beat <= (double) points.getFirst()[ids::b])
        return points.getFirst()[ids::v];

    for (int i = 1; i < points.size(); ++i)
    {
        const double b0 = points[i - 1][ids::b], b1 = points[i][ids::b];

        if (beat <= b1)
        {
            const float v0 = points[i - 1][ids::v], v1 = points[i][ids::v];
            return b1 - b0 < 1.0e-9 ? v1 : v0 + (v1 - v0) * (float) ((beat - b0) / (b1 - b0));
        }
    }

    return points.getLast()[ids::v];
}

te::AutomatableParameter* findParameter (te::Edit& edit, const juce::String& pluginID, const juce::String& paramID)
{
    if (auto plugin = te::findPluginForID (edit, te::EditItemID::fromVar (pluginID)))
        if (auto param = plugin->getAutomatableParameterByID (paramID))
            return param.get();

    return nullptr;
}

te::AutomatableParameter* findAutomationTarget (te::Edit& edit, const juce::ValueTree& clip)
{
    return isAutomationClip (clip) ? findParameter (edit, clip[ids::plugin].toString(), clip[ids::param].toString()) : nullptr;
}

juce::String describeParameter (te::Edit& edit, const juce::String& pluginID, const juce::String& paramID)
{
    auto plugin = te::findPluginForID (edit, te::EditItemID::fromVar (pluginID));
    auto* param = findParameter (edit, pluginID, paramID);

    if (plugin == nullptr || param == nullptr)
        return "(control not here)";

    juce::String owner = "Master";

    if (auto* track = plugin->getOwnerTrack())
        if (! track->isMasterTrack())
            owner = track->getName();

    auto name = param->getParameterName();

    if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin.get()) == nullptr)
        name = plugin->getName() + ": " + name;
    else
        name = name.substring (0, 1).toUpperCase() + name.substring (1);

    return owner + " - " + name;
}

juce::String describeAutomationTarget (te::Edit& edit, const juce::ValueTree& clip)
{
    return describeParameter (edit, clip[ids::plugin].toString(), clip[ids::param].toString());
}

juce::StringArray getAutomatedPlugins (te::Edit& edit)
{
    juce::StringArray result;

    for (auto& c : getClips (edit))
        if (isAutomationClip (c))
            result.addIfNotAlreadyThere (c[ids::plugin].toString());

    return result;
}

//==============================================================================
static juce::ValueTree findLaneNode (te::Edit& edit, int lane)
{
    for (auto l : getModel (edit).getChildWithName (ids::LANES))
        if (l.hasType (ids::LANE) && (int) l[ids::lane] == lane)
            return l;

    return {};
}

/** Sets one setting of a playlist track; the track's entry goes when it has none left. */
static void setLaneProperty (te::Edit& edit, int lane, const juce::Identifier& id, const juce::String& value)
{
    if (! juce::isPositiveAndBelow (lane, numLanes))
        return;

    auto l = findLaneNode (edit, lane);

    if (l.isValid())
    {
        if (l[id].toString() == value)
            return;

        if (value.isEmpty())
            l.removeProperty (id, um (edit));
        else
            l.setProperty (id, value, um (edit));

        // Nothing left to remember about this track: drop its entry.
        if (l[ids::name].toString().isEmpty() && l[ids::colour].toString().isEmpty() && l[ids::insert].toString().isEmpty()
             && l[ids::muted].toString().isEmpty())
            l.getParent().removeChild (l, um (edit));

        return;
    }

    if (value.isEmpty())
        return;

    auto model = getModel (edit);
    auto list = model.getChildWithName (ids::LANES);

    if (! list.isValid())
    {
        list = juce::ValueTree (ids::LANES);
        model.appendChild (list, um (edit));
    }

    juce::ValueTree node (ids::LANE);
    node.setProperty (ids::sid, newSyncID(), nullptr);
    node.setProperty (ids::lane, lane, nullptr);
    node.setProperty (id, value, nullptr);
    list.appendChild (node, um (edit));
}

juce::String getLaneName (te::Edit& edit, int lane)
{
    return findLaneNode (edit, lane)[ids::name].toString();
}

void setLaneName (te::Edit& edit, int lane, const juce::String& nameIn)
{
    setLaneProperty (edit, lane, ids::name, nameIn.trim().substring (0, 60));
}

juce::Colour getLaneColour (te::Edit& edit, int lane)
{
    const auto c = findLaneNode (edit, lane)[ids::colour].toString();
    return c.isNotEmpty() ? juce::Colour::fromString (c) : juce::Colours::transparentBlack;
}

void setLaneColour (te::Edit& edit, int lane, juce::Colour colour)
{
    setLaneProperty (edit, lane, ids::colour, colour.isTransparent() ? juce::String() : colour.withAlpha (1.0f).toString());
}

juce::String getLaneInsertSetting (te::Edit& edit, int lane)
{
    return findLaneNode (edit, lane)[ids::insert].toString();
}

void setLaneInsertSetting (te::Edit& edit, int lane, const juce::String& setting)
{
    setLaneProperty (edit, lane, ids::insert, setting);
}

bool isLaneMuted (te::Edit& edit, int lane)
{
    return (bool) findLaneNode (edit, lane)[ids::muted];
}

void setLaneMuted (te::Edit& edit, int lane, bool muted)
{
    setLaneProperty (edit, lane, ids::muted, muted ? "1" : "");
}

//==============================================================================
static juce::ValueTree getOrCreateList (te::Edit& edit, const juce::Identifier& type)
{
    auto model = getModel (edit);
    auto list = model.getChildWithName (type);

    if (! list.isValid())
    {
        list = juce::ValueTree (type);
        model.appendChild (list, um (edit));
    }

    return list;
}

static LaneGroup laneGroupFrom (const juce::ValueTree& g)
{
    LaneGroup r;
    r.sid = g[ids::sid].toString();
    r.name = g[ids::name].toString();
    const auto c = g[ids::colour].toString();
    r.colour = c.isNotEmpty() ? juce::Colour::fromString (c) : juce::Colours::transparentBlack;
    r.first = juce::jlimit (0, numLanes - 1, (int) g[ids::first]);
    r.count = juce::jlimit (1, numLanes - r.first, (int) g.getProperty (ids::count, 1));
    r.muted = (bool) g[ids::muted];
    return r;
}

static juce::ValueTree findLaneGroupNode (te::Edit& edit, const juce::String& sid)
{
    if (sid.isNotEmpty())
        for (auto g : getModel (edit).getChildWithName (ids::LANEGROUPS))
            if (g.hasType (ids::LANEGROUP) && g[ids::sid].toString() == sid)
                return g;

    return {};
}

juce::Array<LaneGroup> getLaneGroups (te::Edit& edit)
{
    juce::Array<LaneGroup> result;

    for (auto g : getModel (edit).getChildWithName (ids::LANEGROUPS))
        if (g.hasType (ids::LANEGROUP) && g[ids::sid].toString().isNotEmpty())
            result.add (laneGroupFrom (g));

    std::sort (result.begin(), result.end(), [] (const LaneGroup& a, const LaneGroup& b)
               { return a.first != b.first ? a.first < b.first : a.sid < b.sid; });

    // Two people may group overlapping tracks at the same moment: the first one wins (on
    // every PC alike), the other is left out until it's moved or removed.
    juce::Array<LaneGroup> clean;

    for (auto& g : result)
        if (clean.isEmpty() || g.first > clean.getLast().last())
            clean.add (g);

    return clean;
}

LaneGroup getLaneGroup (te::Edit& edit, const juce::String& sid)
{
    for (auto& g : getLaneGroups (edit))
        if (g.sid == sid)
            return g;

    return {};
}

LaneGroup getLaneGroupOf (te::Edit& edit, int lane)
{
    for (auto& g : getLaneGroups (edit))
        if (g.contains (lane))
            return g;

    return {};
}

bool canGroupLanes (te::Edit& edit, int first, int count, const juce::String& exceptGroup)
{
    if (count < 1 || first < 0 || first + count > numLanes)
        return false;

    for (auto& g : getLaneGroups (edit))
        if (g.sid != exceptGroup && first <= g.last() && g.first <= first + count - 1)
            return false;

    return true;
}

juce::String addLaneGroup (te::Edit& edit, int first, int count, const juce::String& name, juce::Colour colour)
{
    if (! canGroupLanes (edit, first, count))
        return {};

    juce::ValueTree g (ids::LANEGROUP);
    const auto sid = newSyncID();
    g.setProperty (ids::sid, sid, nullptr);
    g.setProperty (ids::name, name.trim().substring (0, 60), nullptr);
    g.setProperty (ids::first, first, nullptr);
    g.setProperty (ids::count, count, nullptr);

    if (! colour.isTransparent())
        g.setProperty (ids::colour, colour.withAlpha (1.0f).toString(), nullptr);

    getOrCreateList (edit, ids::LANEGROUPS).appendChild (g, um (edit));
    return sid;
}

void removeLaneGroup (te::Edit& edit, const juce::String& sid)
{
    auto g = findLaneGroupNode (edit, sid);

    if (g.isValid())
        g.getParent().removeChild (g, um (edit));
}

void setLaneGroupName (te::Edit& edit, const juce::String& sid, const juce::String& name)
{
    auto g = findLaneGroupNode (edit, sid);

    if (g.isValid())
        g.setProperty (ids::name, name.trim().substring (0, 60), um (edit));
}

void setLaneGroupColour (te::Edit& edit, const juce::String& sid, juce::Colour colour)
{
    auto g = findLaneGroupNode (edit, sid);

    if (! g.isValid())
        return;

    if (colour.isTransparent())
        g.removeProperty (ids::colour, um (edit));
    else
        g.setProperty (ids::colour, colour.withAlpha (1.0f).toString(), um (edit));
}

void setLaneGroupMuted (te::Edit& edit, const juce::String& sid, bool muted)
{
    auto g = findLaneGroupNode (edit, sid);

    if (! g.isValid() || (bool) g[ids::muted] == muted)
        return;

    if (muted)
        g.setProperty (ids::muted, 1, um (edit));
    else
        g.removeProperty (ids::muted, um (edit));
}

bool setLaneGroupRange (te::Edit& edit, const juce::String& sid, int first, int count)
{
    auto g = findLaneGroupNode (edit, sid);

    if (! g.isValid() || ! canGroupLanes (edit, first, count, sid))
        return false;

    g.setProperty (ids::first, first, um (edit));
    g.setProperty (ids::count, count, um (edit));
    return true;
}

std::vector<bool> getAudibleLanes (te::Edit& edit)
{
    std::vector<bool> audible ((size_t) numLanes, true);

    for (auto l : getModel (edit).getChildWithName (ids::LANES))
        if (l.hasType (ids::LANE) && (bool) l[ids::muted] && juce::isPositiveAndBelow ((int) l[ids::lane], numLanes))
            audible[(size_t) (int) l[ids::lane]] = false;

    for (auto& g : getLaneGroups (edit))
        if (g.muted)
            for (int i = g.first; i <= g.last(); ++i)
                audible[(size_t) i] = false;

    return audible;
}

bool isLaneAudible (te::Edit& edit, int lane)
{
    if (! juce::isPositiveAndBelow (lane, numLanes) || isLaneMuted (edit, lane))
        return false;

    const auto g = getLaneGroupOf (edit, lane);
    return ! (g.isValid() && g.muted);
}

//==============================================================================
juce::Array<juce::ValueTree> getMarkers (te::Edit& edit)
{
    juce::Array<juce::ValueTree> result;

    for (auto m : getModel (edit).getChildWithName (ids::MARKERS))
        if (m.hasType (ids::MARKER))
            result.add (m);

    std::stable_sort (result.begin(), result.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                      { return getMarkerBeat (a) != getMarkerBeat (b) ? getMarkerBeat (a) < getMarkerBeat (b)
                                                                      : a[ids::sid].toString() < b[ids::sid].toString(); });
    return result;
}

juce::ValueTree findMarker (te::Edit& edit, const juce::String& sid)
{
    for (auto m : getModel (edit).getChildWithName (ids::MARKERS))
        if (m.hasType (ids::MARKER) && m[ids::sid].toString() == sid)
            return m;

    return {};
}

juce::ValueTree addMarker (te::Edit& edit, double beat, const juce::String& text, const juce::String& author, juce::Colour colour)
{
    juce::ValueTree m (ids::MARKER);
    m.setProperty (ids::sid, newSyncID(), nullptr);
    m.setProperty (ids::b, juce::jmax (0.0, beat), nullptr);
    m.setProperty (ids::text, text.trim().substring (0, 120), nullptr);

    if (author.isNotEmpty())
        m.setProperty (ids::author, author.substring (0, 60), nullptr);

    if (! colour.isTransparent())
        m.setProperty (ids::colour, colour.withAlpha (1.0f).toString(), nullptr);

    getOrCreateList (edit, ids::MARKERS).appendChild (m, um (edit));
    return m;
}

void setMarkerText (te::Edit& edit, juce::ValueTree m, const juce::String& text)
{
    if (m.isValid())
        m.setProperty (ids::text, text.trim().substring (0, 120), um (edit));
}

void setMarkerBeat (te::Edit& edit, juce::ValueTree m, double beat)
{
    if (m.isValid() && std::abs (getMarkerBeat (m) - beat) > 1.0e-9)
        m.setProperty (ids::b, juce::jmax (0.0, beat), um (edit));
}

void setMarkerColour (te::Edit& edit, juce::ValueTree m, juce::Colour colour)
{
    if (! m.isValid())
        return;

    if (colour.isTransparent())
        m.removeProperty (ids::colour, um (edit));
    else
        m.setProperty (ids::colour, colour.withAlpha (1.0f).toString(), um (edit));
}

void removeMarker (te::Edit& edit, juce::ValueTree m)
{
    if (m.isValid() && m.getParent().isValid())
        m.getParent().removeChild (m, um (edit));
}

double getMarkerBeat (const juce::ValueTree& m)        { return (double) m[ids::b]; }

juce::Colour getMarkerColour (const juce::ValueTree& m)
{
    const auto c = m[ids::colour].toString();
    return c.isNotEmpty() ? juce::Colour::fromString (c) : juce::Colours::transparentBlack;
}

te::AudioTrack* findInsertNamed (te::Edit& edit, const juce::String& name)
{
    const auto wanted = name.trim();

    if (wanted.isEmpty())
        return nullptr;

    for (auto* t : getInserts (edit))
        if (t->getName().trim().equalsIgnoreCase (wanted))
            return t;

    return nullptr;
}

LaneTarget getLaneTarget (te::Edit& edit, int lane)
{
    const auto setting = getLaneInsertSetting (edit, lane);

    if (setting == laneKeepsInsert)
        return {};

    if (setting == laneToMaster)
        return { true, {} };

    if (setting.isNotEmpty())
    {
        if (auto* t = findInsert (edit, setting))
            return { true, t->itemID.toString() };

        return {};   // that insert was deleted
    }

    // Not set: the insert with the track's name, if there is one (FL templates name them alike).
    if (auto* t = findInsertNamed (edit, getLaneName (edit, lane)))
        return { true, t->itemID.toString() };

    return {};
}

bool routeTakeToLane (te::Edit& edit, const juce::ValueTree& clip)
{
    if (! isAudioClip (clip))
        return false;

    const auto target = getLaneTarget (edit, clip[ids::lane]);
    auto* channel = findChannel (edit, clip[ids::ch].toString());

    if (! target.routes || channel == nullptr || getRoute (*channel) == target.insertID)
        return false;

    if (! canRouteTo (edit, *channel, target.insertID))
        return false;

    // Other clips of the same channel (a duplicated clip) that stay on tracks going
    // somewhere else keep their insert: this clip gets a channel of its own first.
    const auto channelID = getChannelID (*channel);

    for (auto& other : getClips (edit))
    {
        if (other == clip || ! isAudioClip (other) || other[ids::ch].toString() != channelID)
            continue;

        const auto there = getLaneTarget (edit, other[ids::lane]);

        if (! there.routes || there.insertID != target.insertID)
        {
            channel = makeAudioClipUnique (edit, clip);

            if (channel == nullptr)
                return false;

            break;
        }
    }

    return setRoute (edit, *channel, target.insertID);
}

te::AudioTrack* makeAudioClipUnique (te::Edit& edit, const juce::ValueTree& clip)
{
    if (! isAudioClip (clip))
        return nullptr;

    auto* channel = findChannel (edit, clip[ids::ch].toString());

    if (channel == nullptr || countClipsOfChannel (edit, getChannelID (*channel)) < 2)
        return nullptr;   // nothing to separate it from

    // "Name #2", "Name #3"... the first free one.
    const auto base = channel->getName().upToLastOccurrenceOf (" #", false, false).isNotEmpty()
                      && channel->getName().fromLastOccurrenceOf (" #", false, false).containsOnly ("0123456789")
                        ? channel->getName().upToLastOccurrenceOf (" #", false, false) : channel->getName();
    juce::StringArray names;

    for (auto* t : getChannels (edit))
        names.add (t->getName());

    int n = 2;

    while (names.contains (base + " #" + juce::String (n)))
        ++n;

    auto* copy = addAudioChannel (edit, base + " #" + juce::String (n));

    if (copy == nullptr)
        return nullptr;

    // Same file, same insert, same volume and pan: it sounds the same until you change it.
    copy->state.setProperty (ids::asset, getChannelAsset (edit, *channel), um (edit));

    if (getRoute (*channel).isNotEmpty())
        setRoute (edit, *copy, getRoute (*channel));

    if (auto* from = getVolumePlugin (*channel))
        if (auto* to = getVolumePlugin (*copy))
        {
            to->volParam->setParameter (from->volParam->getCurrentValue(), juce::sendNotification);
            to->panParam->setParameter (from->panParam->getCurrentValue(), juce::sendNotification);
        }

    auto c = clip;
    c.setProperty (ids::ch, getChannelID (*copy), um (edit));
    return copy;
}

int countClipsOfChannel (te::Edit& edit, const juce::String& channelID)
{
    int n = 0;

    for (auto& c : getClips (edit))
        if (isAudioClip (c) && c[ids::ch].toString() == channelID)
            ++n;

    return n;
}

//==============================================================================
static juce::ValueTree getSendList (te::Edit& edit, bool createIfMissing)
{
    auto model = getModel (edit);
    auto list = model.getChildWithName (ids::SENDS);

    if (! list.isValid() && createIfMissing)
    {
        list = juce::ValueTree (ids::SENDS);
        model.appendChild (list, um (edit));
    }

    return list;
}

juce::Array<juce::ValueTree> getSends (te::Edit& edit)
{
    juce::Array<juce::ValueTree> result;

    for (auto s : getSendList (edit, false))
        if (s.hasType (ids::SEND))
            result.add (s);

    return result;
}

juce::Array<juce::ValueTree> getSendsFrom (te::Edit& edit, const juce::String& insertID)
{
    juce::Array<juce::ValueTree> result;

    for (auto s : getSends (edit))
        if (s[ids::from].toString() == insertID)
            result.add (s);

    return result;
}

bool canSendTo (te::Edit& edit, const juce::String& fromID, const juce::String& toID)
{
    if (fromID.isEmpty() || toID.isEmpty() || fromID == toID
         || findInsert (edit, fromID) == nullptr || findInsert (edit, toID) == nullptr)
        return false;

    for (auto s : getSendsFrom (edit, fromID))
        if (s[ids::to].toString() == toID)
            return false;   // already sending there

    return ! feeds (edit, toID, fromID);   // the target's sound must not come back
}

juce::ValueTree addSend (te::Edit& edit, const juce::String& fromID, const juce::String& toID, double db)
{
    if (! canSendTo (edit, fromID, toID))
        return {};

    juce::ValueTree s (ids::SEND);
    s.setProperty (ids::sid, newSyncID(), nullptr);
    s.setProperty (ids::from, fromID, nullptr);
    s.setProperty (ids::to, toID, nullptr);
    s.setProperty (ids::db, juce::jlimit (minSendDb, maxSendDb, db), nullptr);
    getSendList (edit, true).appendChild (s, um (edit));
    return s;
}

void removeSend (te::Edit& edit, juce::ValueTree send)
{
    auto parent = send.getParent();

    if (parent.isValid())
        parent.removeChild (send, um (edit));
}

void setSendLevel (te::Edit& edit, juce::ValueTree send, double db)
{
    send.setProperty (ids::db, juce::jlimit (minSendDb, maxSendDb, db), um (edit));
}

double getSendLevel (const juce::ValueTree& send)
{
    return juce::jlimit (minSendDb, maxSendDb, (double) send.getProperty (ids::db, 0.0));
}

te::AudioTrack* resolveRoute (te::Edit& edit, const te::AudioTrack& track)
{
    auto target = getRoute (track);
    return canRouteTo (edit, track, target) ? findInsert (edit, target) : nullptr;
}

bool setRoute (te::Edit& edit, te::AudioTrack& source, const juce::String& targetID)
{
    if (! canRouteTo (edit, source, targetID))
        return false;

    source.state.setProperty (ids::routeTo, targetID, um (edit));
    return true;
}

juce::Array<te::Plugin*> getInsertEffects (te::AudioTrack& track)
{
    juce::Array<te::Plugin*> result;

    for (auto* p : track.pluginList.getPlugins())
        if (! isNotAnEffect (p))
            result.add (p);

    return result;
}

te::Plugin::Ptr addInsertEffect (te::AudioTrack& track, const juce::PluginDescription& desc)
{
    auto plugin = track.edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc);

    if (plugin == nullptr)
        return {};

    int index = 0;
    auto plugins = track.pluginList.getPlugins();

    while (index < plugins.size() && ! isMixerUtility (plugins[index].get()))
        ++index;

    track.pluginList.insertPlugin (plugin, index, nullptr);
    return plugin;
}

juce::Array<te::Plugin*> getMasterEffects (te::Edit& edit)
{
    juce::Array<te::Plugin*> result;

    for (auto* p : edit.getMasterPluginList().getPlugins())
        if (! isMixerUtility (p))
            result.add (p);

    return result;
}

te::Plugin::Ptr addMasterEffect (te::Edit& edit, const juce::PluginDescription& desc)
{
    auto plugin = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc);

    if (plugin != nullptr)
        edit.getMasterPluginList().insertPlugin (plugin, -1, nullptr);

    return plugin;
}

juce::Array<std::pair<juce::String, juce::String>> getBuiltInEffects()
{
    juce::Array<std::pair<juce::String, juce::String>> list;
    list.add ({ ParametricEQPlugin::xmlTypeName,   "Parametric EQ (7 band)" });
    list.add ({ LimiterPlugin::xmlTypeName,        "Limiter" });
    list.add ({ DelayPlugin::xmlTypeName,          "Delay" });
    list.add ({ SidechainPlugin::xmlTypeName,      "Sidechain (duck to the kick)" });
    list.add ({ ReverbPlugin::xmlTypeName,         "Reverb" });
    list.add ({ CompressorFxPlugin::xmlTypeName,   "Compressor" });
    list.add ({ EnhancerPlugin::xmlTypeName,       "Enhancer (A-D: fuller, louder)" });
    list.add ({ FlangerPlugin::xmlTypeName,        "Flanger" });
    list.add ({ FilterFxPlugin::xmlTypeName,       "Filter (low / band / high)" });
    list.add ({ te::EqualiserPlugin::xmlTypeName,  "Equaliser (4 band)" });
    list.add ({ te::CompressorPlugin::xmlTypeName, "Compressor (simple)" });
    list.add ({ te::ReverbPlugin::xmlTypeName,     "Reverb (simple)" });
    list.add ({ te::DelayPlugin::xmlTypeName,      "Delay (simple)" });
    list.add ({ te::ChorusPlugin::xmlTypeName,     "Chorus" });
    list.add ({ te::PhaserPlugin::xmlTypeName,     "Phaser" });
    list.add ({ te::LowPassPlugin::xmlTypeName,    "Low / high pass filter" });
    return list;
}

te::Plugin::Ptr createEffect (te::Edit& edit, const juce::String& builtInType, const juce::PluginDescription* external)
{
    if (external != nullptr)
        return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *external);

    return edit.getPluginCache().createNewPlugin (builtInType, {});
}

void putEffect (te::Edit& edit, te::AudioTrack* insert, te::Plugin::Ptr plugin, int slot)
{
    if (plugin == nullptr)
        return;

    auto& list = insert != nullptr ? insert->pluginList : edit.getMasterPluginList();
    auto effects = insert != nullptr ? getInsertEffects (*insert) : getMasterEffects (edit);
    auto all = list.getPlugins();
    int index = -1;

    if (juce::isPositiveAndBelow (slot, effects.size()))
    {
        index = all.indexOf (effects[slot]);
    }
    else
    {
        // After the last effect, before volume/meter.
        for (int i = 0; i < all.size(); ++i)
            if (isMixerUtility (all[i].get()))
                { index = i; break; }
    }

    list.insertPlugin (plugin, index, nullptr);
}

void moveEffect (te::Plugin& plugin, int direction)
{
    auto node = plugin.state;
    auto parent = node.getParent();

    if (! parent.isValid())
        return;

    // Find the neighbouring PLUGIN node (skipping other kinds of children).
    const int from = parent.indexOf (node);

    for (int i = from + direction; i >= 0 && i < parent.getNumChildren(); i += direction)
    {
        auto other = parent.getChild (i);

        if (! other.hasType (te::IDs::PLUGIN))
            continue;

        if (auto otherPlugin = te::findPluginForID (plugin.edit, te::EditItemID::fromVar (other[te::IDs::id])))
            if (isNotAnEffect (otherPlugin.get()))
                return;     // never move past volume/meter or the sends' return

        parent.moveChild (from, i, &plugin.edit.getUndoManager());
        return;
    }
}

//==============================================================================
static const juce::String audioKind ("audio");

bool isAudioChannel (const te::AudioTrack& track)
{
    return track.state[ids::kind].toString() == audioKind;
}

te::AudioTrack* addAudioChannel (te::Edit& edit, const juce::String& name)
{
    auto* track = addEmptyChannel (edit, name.isNotEmpty() ? name : juce::String ("Audio"));

    if (track != nullptr)
        track->state.setProperty (ids::kind, audioKind, um (edit));

    return track;
}

static juce::ValueTree getAssetsTree (te::Edit& edit, bool create)
{
    auto model = getModel (edit);
    auto assets = model.getChildWithName (ids::ASSETS);

    if (! assets.isValid() && create)
    {
        assets = juce::ValueTree (ids::ASSETS);
        model.appendChild (assets, um (edit));
    }

    return assets;
}

static juce::ValueTree findAsset (te::Edit& edit, const juce::String& hash)
{
    for (auto a : getAssetsTree (edit, false))
        if (a[ids::sid].toString() == hash)
            return a;

    return {};
}

void addAsset (te::Edit& edit, const AssetStore::ImportResult& r, const juce::String& name)
{
    if (findAsset (edit, r.hash).isValid())
        return;     // the same file is already in the song

    juce::ValueTree a (ids::ASSET);
    a.setProperty (ids::sid, r.hash, nullptr);
    a.setProperty (ids::name, name, nullptr);
    a.setProperty (ids::seconds, r.lengthSeconds, nullptr);
    a.setProperty (ids::rate, r.sampleRate, nullptr);
    a.setProperty (ids::numChannels, r.numChannels, nullptr);
    a.setProperty (ids::bytes, r.bytes, nullptr);
    getAssetsTree (edit, true).appendChild (a, um (edit));
}

juce::String getAssetName (te::Edit& edit, const juce::String& hash)
{
    return findAsset (edit, hash)[ids::name].toString();
}

bool isTakeAsset (te::Edit& edit, const juce::String& hash)
{
    auto asset = findAsset (edit, hash);

    if ((bool) asset["take"])
        return true;

    // Songs from before takes were marked: "<insert> take <n>.flac".
    const auto name = asset[ids::name].toString();
    return name.endsWith (".flac") && name.upToLastOccurrenceOf (".flac", false, false)
                                          .fromLastOccurrenceOf (" take ", false, false).containsOnly ("0123456789")
        && name.contains (" take ");
}

double getAssetSeconds (te::Edit& edit, const juce::String& hash)
{
    return (double) findAsset (edit, hash).getProperty (ids::seconds, 0.0);
}

juce::StringArray getAssetHashes (te::Edit& edit)
{
    juce::StringArray hashes;

    for (auto a : getAssetsTree (edit, false))
        hashes.addIfNotAlreadyThere (a[ids::sid].toString());

    for (auto& c : getClips (edit))
        if (isAudioClip (c))
            hashes.addIfNotAlreadyThere (c[ids::asset].toString());

    for (auto* t : getChannels (edit))
        hashes.addIfNotAlreadyThere (getSamplerAsset (*t));

    hashes.removeEmptyStrings();
    return hashes;
}

bool isAudioClip (const juce::ValueTree& clip)
{
    return clip[ids::kind].toString() == audioKind;
}

juce::ValueTree addAudioClip (te::Edit& edit, const juce::String& hash, const juce::String& channelID,
                              int lane, double startBeat, double offsetSeconds, double durationSeconds)
{
    juce::ValueTree c (ids::PCLIP);
    c.setProperty (ids::sid, newSyncID(), nullptr);
    c.setProperty (ids::kind, audioKind, nullptr);
    c.setProperty (ids::asset, hash, nullptr);
    c.setProperty (ids::ch, channelID, nullptr);
    c.setProperty (ids::lane, juce::jlimit (0, numLanes - 1, lane), nullptr);
    c.setProperty (ids::start, juce::jmax (0.0, startBeat), nullptr);
    c.setProperty (ids::offset, juce::jmax (0.0, offsetSeconds), nullptr);
    c.setProperty (ids::duration, juce::jmax (0.01, durationSeconds), nullptr);
    getPlaylist (edit, true).appendChild (c, um (edit));
    return c;
}

juce::String getChannelAsset (te::Edit& edit, const te::AudioTrack& channel)
{
    auto hash = channel.state[ids::asset].toString();

    if (hash.isNotEmpty())
        return hash;

    // Older songs: find it through one of the channel's clips.
    const auto channelID = getChannelID (channel);

    for (auto& c : getClips (edit))
        if (isAudioClip (c) && c[ids::ch].toString() == channelID)
            return c[ids::asset].toString();

    return {};
}

juce::ValueTree placeAudioChannel (te::Edit& edit, const te::AudioTrack& channel, int lane, double startBeat)
{
    const auto hash = getChannelAsset (edit, channel);

    if (! isAudioChannel (channel) || hash.isEmpty())
        return {};

    return addAudioClip (edit, hash, getChannelID (channel), lane, startBeat, 0.0,
                         juce::jmax (0.05, getAssetSeconds (edit, hash)));
}

//==============================================================================
juce::String channelNameForFile (const juce::String& fileName)
{
    const auto base = fileName.upToLastOccurrenceOf (".", false, false);
    return (base.isNotEmpty() ? base : fileName).substring (0, 40);
}

static SamplerPlugin* getSampler (te::AudioTrack& track)
{
    return dynamic_cast<SamplerPlugin*> (getInstrument (track));
}

bool isSamplerChannel (te::AudioTrack& track)               { return getSampler (track) != nullptr; }

juce::String getSamplerAsset (te::AudioTrack& track)
{
    auto* sampler = getSampler (track);
    return sampler != nullptr ? sampler->getAsset() : juce::String();
}

te::AudioTrack* addSamplerChannel (te::Edit& edit, const AssetStore::ImportResult& r, const juce::String& fileName)
{
    addAsset (edit, r, fileName);
    auto* track = addEmptyChannel (edit, channelNameForFile (fileName));

    if (track != nullptr)
    {
        if (auto p = edit.getPluginCache().createNewPlugin (SamplerPlugin::xmlTypeName, {}))
        {
            p->state.setProperty (SamplerPlugin::assetID, r.hash, nullptr);   // travels with the plugin
            track->pluginList.insertPlugin (p, 0, nullptr);
        }
    }

    return track;
}

te::AudioTrack* addEmptySamplerChannel (te::Edit& edit, const juce::String& name)
{
    auto* track = addEmptyChannel (edit, name);

    if (track != nullptr)
        if (auto p = edit.getPluginCache().createNewPlugin (SamplerPlugin::xmlTypeName, {}))
            track->pluginList.insertPlugin (p, 0, nullptr);

    return track;
}

bool setSamplerSample (te::Edit& edit, te::AudioTrack& track, const AssetStore::ImportResult& r, const juce::String& fileName)
{
    auto* sampler = getSampler (track);

    if (sampler == nullptr)
        return false;

    addAsset (edit, r, fileName);
    sampler->state.setProperty (SamplerPlugin::assetID, r.hash, um (edit));
    track.setName (channelNameForFile (fileName));
    return true;
}

juce::StringArray getSamplerChannelsUsing (te::Edit& edit, const juce::String& hash)
{
    juce::StringArray names;

    for (auto* t : getChannels (edit))
        if (getSamplerAsset (*t) == hash)
            names.add (t->getName());

    return names;
}

static bool isLaneFree (te::Edit& edit, const juce::Array<juce::ValueTree>& clips, int lane, double startBeat, double endBeat)
{
    for (auto& c : clips)
        if ((int) c[ids::lane] == lane && (double) c[ids::start] < endBeat
             && (double) c[ids::start] + getClipLength (edit, c) > startBeat)
            return false;

    return true;
}

int findFreeLane (te::Edit& edit, double startBeat, double endBeat)
{
    const auto clips = getClips (edit);

    for (int lane = 0; lane < numLanes; ++lane)
        if (isLaneFree (edit, clips, lane, startBeat, endBeat))
            return lane;

    return numLanes - 1;
}

int findLaneForTake (te::Edit& edit, const juce::String& insertID, double startBeat, double endBeat)
{
    const auto clips = getClips (edit);

    // A free track that sends its takes to this insert...
    for (int lane = 0; lane < numLanes; ++lane)
    {
        const auto target = getLaneTarget (edit, lane);

        if (target.routes && target.insertID == insertID && isLaneFree (edit, clips, lane, startBeat, endBeat))
            return lane;
    }

    // ...otherwise a free one that doesn't send takes elsewhere.
    for (int lane = 0; lane < numLanes; ++lane)
        if (! getLaneTarget (edit, lane).routes && isLaneFree (edit, clips, lane, startBeat, endBeat))
            return lane;

    return findFreeLane (edit, startBeat, endBeat);
}

juce::ValueTree addRecordedTake (te::Edit& edit, const AssetStore::ImportResult& r, const juce::String& insertID,
                                 double startSeconds, double offsetSeconds, double durationSeconds)
{
    auto* insert = findInsert (edit, insertID);
    const auto base = (insert != nullptr ? insert->getName() : juce::String ("Recording")) + " take ";

    // Next free take number for this insert.
    int take = 1;

    for (auto* t : getChannels (edit))
        if (t->getName().startsWith (base))
            take = juce::jmax (take, t->getName().fromFirstOccurrenceOf (base, false, false).getIntValue() + 1);

    const auto name = base + juce::String (take);
    addAsset (edit, r, name + ".flac");
    findAsset (edit, r.hash).setProperty ("take", true, um (edit));   // goes in the project's Takes folder

    auto* channel = addAudioChannel (edit, name);

    if (channel == nullptr)
        return {};

    channel->state.setProperty (ids::asset, r.hash, um (edit));

    if (insert != nullptr)
        setRoute (edit, *channel, insertID);   // plays through the same effects you heard

    const double startBeat = secondsToBeats (edit, startSeconds);
    const double endBeat = secondsToBeats (edit, startSeconds + durationSeconds);
    return addAudioClip (edit, r.hash, getChannelID (*channel),
                         findLaneForTake (edit, insert != nullptr ? insertID : juce::String(), startBeat, endBeat),
                         startBeat, offsetSeconds, durationSeconds);
}

juce::ValueTree addImportedAudio (te::Edit& edit, const AssetStore::ImportResult& r, const juce::String& name,
                                  int lane, double startBeat)
{
    addAsset (edit, r, name);
    auto* channel = addAudioChannel (edit, name.upToLastOccurrenceOf (".", false, false).substring (0, 40));

    if (channel == nullptr)
        return {};

    channel->state.setProperty (ids::asset, r.hash, um (edit));
    return addAudioClip (edit, r.hash, getChannelID (*channel), lane, startBeat, 0.0, r.lengthSeconds);
}

double beatsToSeconds (te::Edit& edit, double beats)
{
    return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beats)).inSeconds();
}

double secondsToBeats (te::Edit& edit, double seconds)
{
    return edit.tempoSequence.toBeats (te::TimePosition::fromSeconds (seconds)).inBeats();
}

//==============================================================================
juce::Array<juce::ValueTree> getAssets (te::Edit& edit)
{
    juce::Array<juce::ValueTree> list;

    for (auto a : getAssetsTree (edit, false))
        if (a.hasType (ids::ASSET))
            list.add (a);

    return list;
}

int getAssetUseCount (te::Edit& edit, const juce::String& hash)
{
    int n = 0;

    for (auto& c : getClips (edit))
        if (isAudioClip (c) && c[ids::asset].toString() == hash)
            ++n;

    return n;
}

void renameAsset (te::Edit& edit, const juce::String& hash, const juce::String& newName)
{
    auto asset = findAsset (edit, hash);
    const auto name = newName.trim().substring (0, 60);

    if (! asset.isValid() || name.isEmpty())
        return;

    asset.setProperty (ids::name, name, um (edit));

    for (auto* t : getChannels (edit))
        if (isAudioChannel (*t) && getChannelAsset (edit, *t) == hash)
            t->setName (name.upToLastOccurrenceOf (".", false, false).isNotEmpty()
                          ? name.upToLastOccurrenceOf (".", false, false) : name);
}

te::AudioTrack* findOrCreateChannelForAsset (te::Edit& edit, const juce::String& hash)
{
    for (auto* t : getChannels (edit))
        if (isAudioChannel (*t) && getChannelAsset (edit, *t) == hash)
            return t;

    const auto name = getAssetName (edit, hash);
    auto* channel = addAudioChannel (edit, name.upToLastOccurrenceOf (".", false, false).isNotEmpty()
                                             ? name.upToLastOccurrenceOf (".", false, false) : name);

    if (channel != nullptr)
        channel->state.setProperty (ids::asset, hash, um (edit));

    return channel;
}

void removeAsset (te::Edit& edit, const juce::String& hash)
{
    // Its channels (and with them their clips)...
    juce::Array<te::AudioTrack*> channels;

    for (auto* t : getChannels (edit))
        if ((isAudioChannel (*t) && getChannelAsset (edit, *t) == hash) || getSamplerAsset (*t) == hash)
            channels.add (t);

    for (auto* t : channels)
        removeChannel (edit, *t);

    // ...any clip still playing it, and its entry.
    for (auto& c : getClips (edit))
        if (isAudioClip (c) && c[ids::asset].toString() == hash)
            removeClip (edit, c);

    auto asset = findAsset (edit, hash);

    if (asset.isValid())
        asset.getParent().removeChild (asset, um (edit));
}

juce::StringArray findUnusedTakes (te::Edit& edit)
{
    juce::StringArray result;

    for (auto& a : getAssets (edit))
    {
        const auto hash = a[ids::sid].toString();

        if (isTakeAsset (edit, hash) && getAssetUseCount (edit, hash) == 0 && getSamplerChannelsUsing (edit, hash).isEmpty())
            result.add (hash);
    }

    return result;
}

juce::StringArray getPatternChannels (te::Edit& edit, const juce::ValueTree& pattern)
{
    juce::StringArray ids_;

    for (auto* t : getChannels (edit))
        if (! getNotes (pattern, getChannelID (*t)).isEmpty())
            ids_.add (getChannelID (*t));

    return ids_;
}

juce::Array<juce::ValueTree> splitPatternByChannel (te::Edit& edit, const juce::ValueTree& pattern)
{
    juce::Array<juce::ValueTree> result;
    const auto channels = getPatternChannels (edit, pattern);

    if (! pattern.getParent().isValid() || channels.size() < 2)
        return result;

    const auto name = pattern[ids::name].toString();
    const auto oldSid = pattern[ids::sid].toString();
    const int bars = getBars (pattern);

    for (auto& channelID : channels)
    {
        auto* t = findChannel (edit, channelID);
        auto p = addPattern (edit, name + " - " + (t != nullptr ? t->getName() : juce::String ("?")), bars);

        for (auto& n : getNotes (pattern, channelID))
            copyNoteProperties (edit, n, addNote (edit, p, channelID, n[ids::p], n[ids::b], n[ids::l], n[ids::v]));

        result.add (p);
    }

    // Each placement of the old pattern becomes one placement per new pattern: the
    // first on the same track, the others on the next tracks below that are free at
    // that time (so nothing ends up on top of other clips).
    auto laneIsFree = [&] (int lane, double start, double end)
    {
        for (auto& other : getClips (edit))
            if (other[ids::pattern].toString() != oldSid && (int) other[ids::lane] == lane
                 && (double) other[ids::start] < end && (double) other[ids::start] + getClipLength (edit, other) > start)
                return false;

        return true;
    };

    for (auto& c : getClips (edit))
    {
        if (c[ids::pattern].toString() != oldSid)
            continue;

        const int lane = c[ids::lane];
        const double start = c[ids::start];
        const double end = start + getClipLength (edit, c);
        int nextLane = lane;

        for (int i = 0; i < result.size(); ++i)
        {
            int target = i == 0 ? lane : -1;

            for (int l = nextLane; target < 0 && l < numLanes; ++l)
                if (laneIsFree (l, start, end))
                    target = l;

            if (target < 0)
                target = juce::jmin (numLanes - 1, lane + i);   // no free track left: stack below

            nextLane = target + 1;
            auto copy = addClip (edit, result[i][ids::sid].toString(), target, start);

            if (c.hasProperty (ids::length))
                copy.setProperty (ids::length, c[ids::length], um (edit));

            if (c.hasProperty (ids::poff))
                copy.setProperty (ids::poff, c[ids::poff], um (edit));
        }
    }

    removePattern (edit, pattern);   // also removes its old clips
    return result;
}

void createDefaultSong (te::Edit& edit)
{
    // Tracktion creates one empty audio track in a new Edit; we use our own channels.
    for (auto* t : te::getAudioTracks (edit))
        if (! (bool) t->state[ids::channel])
            edit.deleteTrack (t);

    auto* kick  = addChannel (edit, InstrumentKind::kick);
    auto* clap  = addChannel (edit, InstrumentKind::clap);
    auto* hat   = addChannel (edit, InstrumentKind::hat);
    auto* snare = addChannel (edit, InstrumentKind::snare);
    addChannel (edit, InstrumentKind::synth);
    juce::ignoreUnused (snare);

    auto pattern = addPattern (edit, "Pattern 1", 1);

    for (int s : { 0, 4, 8, 12 })     setStep (edit, pattern, *kick, s, true);
    for (int s : { 4, 12 })           setStep (edit, pattern, *clap, s, true);
    for (int s : { 2, 6, 10, 14 })    setStep (edit, pattern, *hat, s, true);

    // The song starts with Pattern 1 placed at bar 1, so SONG mode plays something.
    addClip (edit, pattern[ids::sid].toString(), 0, 0.0);

    edit.getUndoManager().clearUndoHistory();
}

} // namespace song

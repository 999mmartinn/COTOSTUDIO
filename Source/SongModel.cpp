#include "SongModel.h"
#include "DrumSynthPlugin.h"

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

juce::ValueTree addPattern (te::Edit& edit, const juce::String& name, int bars)
{
    static const juce::uint32 colours[] = { 0xff5fb85f, 0xff5f9fd8, 0xffd8a05f, 0xffc66fd0, 0xffd86f6f, 0xff6fd0c6 };

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

void removeClip (te::Edit& edit, juce::ValueTree clip)
{
    auto parent = clip.getParent();

    if (parent.isValid())
        parent.removeChild (clip, um (edit));
}

double getClipLength (te::Edit& edit, const juce::ValueTree& clip)
{
    if (clip.hasProperty (ids::length))
        return juce::jmax (stepBeats, (double) clip[ids::length]);

    auto pattern = findPattern (edit, clip[ids::pattern].toString());
    return pattern.isValid() ? getLengthBeats (pattern) : 4.0;
}

double getSongLengthBeats (te::Edit& edit)
{
    double end = 0.0;

    for (auto& c : getClips (edit))
        end = juce::jmax (end, (double) c[ids::start] + getClipLength (edit, c));

    return juce::jmax (4.0, std::ceil (end / 4.0 - 1.0e-9) * 4.0);
}

int getBars (const juce::ValueTree& pattern)               { return juce::jlimit (1, 8, (int) pattern.getProperty (ids::bars, 1)); }
int getNumSteps (const juce::ValueTree& pattern)           { return getBars (pattern) * stepsPerBar; }
double getLengthBeats (const juce::ValueTree& pattern)     { return getBars (pattern) * 4.0; }

void setBars (te::Edit& edit, juce::ValueTree pattern, int bars)
{
    pattern.setProperty (ids::bars, juce::jlimit (1, 8, bars), um (edit));
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

void removeChannel (te::Edit& edit, te::AudioTrack& track)
{
    auto id = getChannelID (track);

    // Remove its notes from every pattern first.
    for (auto& pattern : getPatterns (edit))
        for (auto& n : getNotes (pattern, id))
            pattern.removeChild (n, um (edit));

    edit.deleteTrack (&track);
}

static bool isMixerUtility (te::Plugin* p)
{
    return dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr;
}

te::Plugin* getInstrument (te::AudioTrack& track)
{
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
        if (p != instrument && ! isMixerUtility (p))
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

void removeNote (te::Edit& edit, juce::ValueTree note)
{
    auto parent = note.getParent();

    if (parent.isValid())
        parent.removeChild (note, um (edit));
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

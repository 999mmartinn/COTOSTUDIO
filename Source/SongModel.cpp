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
    if (isAudioClip (clip))
    {
        const double start = clip[ids::start];
        const double endSeconds = beatsToSeconds (edit, start) + (double) clip[ids::duration];
        return juce::jmax (stepBeats / 4.0, secondsToBeats (edit, endSeconds) - start);
    }

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

    // Remove its notes from every pattern, and its audio clips, first.
    for (auto& pattern : getPatterns (edit))
        for (auto& n : getNotes (pattern, id))
            pattern.removeChild (n, um (edit));

    for (auto& c : getClips (edit))
        if (c[ids::ch].toString() == id)
            removeClip (edit, c);

    edit.deleteTrack (&track);
}

static bool isMixerUtility (te::Plugin* p)
{
    return dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr;
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

    // Follow the chain from the target; reaching the source would make a loop.
    auto* t = findInsert (edit, targetID);

    for (int guard = 0; t != nullptr && guard < 1000; ++guard)
    {
        if (t == &source)
            return false;

        t = findInsert (edit, getRoute (*t));
    }

    return findInsert (edit, targetID) != nullptr;
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
        if (! isMixerUtility (p))
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
    list.add ({ te::EqualiserPlugin::xmlTypeName,  "Equaliser (4 band)" });
    list.add ({ te::CompressorPlugin::xmlTypeName, "Compressor" });
    list.add ({ te::ReverbPlugin::xmlTypeName,     "Reverb" });
    list.add ({ te::DelayPlugin::xmlTypeName,      "Delay" });
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
            if (isMixerUtility (otherPlugin.get()))
                return;     // never move past volume/meter

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

juce::ValueTree addImportedAudio (te::Edit& edit, const AssetStore::ImportResult& r, const juce::String& name,
                                  int lane, double startBeat)
{
    addAsset (edit, r, name);
    auto* channel = addAudioChannel (edit, name.upToLastOccurrenceOf (".", false, false).substring (0, 40));

    if (channel == nullptr)
        return {};

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

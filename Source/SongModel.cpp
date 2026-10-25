#include "SongModel.h"
#include "DrumSynthPlugin.h"
#include "SamplerPlugin.h"

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

juce::ValueTree clonePattern (te::Edit& edit, const juce::ValueTree& pattern)
{
    auto copy = addPattern (edit, pattern[ids::name].toString() + " copy", getBars (pattern));

    for (auto n : pattern)
        if (n.hasType (ids::PNOTE))
            addNote (edit, copy, n[ids::ch].toString(), n[ids::p], n[ids::b], n[ids::l], n[ids::v]);

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

    const double maxBeats = 8 * 4.0;
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

    const int bars = juce::jlimit (1, 8, (int) std::ceil (end / 4.0 - 1.0e-9));

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

int findFreeLane (te::Edit& edit, double startBeat, double endBeat)
{
    for (int lane = 0; lane < numLanes; ++lane)
    {
        bool busy = false;

        for (auto& c : getClips (edit))
            if ((int) c[ids::lane] == lane && (double) c[ids::start] < endBeat
                 && (double) c[ids::start] + getClipLength (edit, c) > startBeat)
                busy = true;

        if (! busy)
            return lane;
    }

    return numLanes - 1;
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
    return addAudioClip (edit, r.hash, getChannelID (*channel), findFreeLane (edit, startBeat, endBeat),
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
            addNote (edit, p, channelID, n[ids::p], n[ids::b], n[ids::l], n[ids::v]);

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
        const double end = start + (c.hasProperty (ids::length) ? juce::jmax (stepBeats, (double) c[ids::length])
                                                               : (double) bars * 4.0);
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

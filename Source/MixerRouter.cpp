#include "MixerRouter.h"

//==============================================================================
juce::String MixerRouter::Solo::toString() const
{
    juce::StringArray parts;

    for (auto& c : channels)  parts.add ("c:" + c);
    for (auto& i : inserts)   parts.add ("i:" + i);

    return parts.joinIntoString (",");
}

MixerRouter::Solo MixerRouter::Solo::fromString (const juce::String& s)
{
    Solo result;

    for (auto& part : juce::StringArray::fromTokens (s, ",", {}))
    {
        if (part.startsWith ("c:"))         result.channels.addIfNotAlreadyThere (part.substring (2));
        else if (part.startsWith ("i:"))    result.inserts.addIfNotAlreadyThere (part.substring (2));
    }

    return result;
}

//==============================================================================
MixerRouter::MixerRouter (te::Edit& e) : edit (e)
{
    edit.state.addListener (this);
    applyNow();
    startTimer (500);
}

MixerRouter::~MixerRouter()
{
    stopTimer();
    cancelPendingUpdate();
    edit.state.removeListener (this);
}

void MixerRouter::setSolo (const Solo& newSolo)
{
    if (newSolo == solo)
        return;

    solo = newSolo;
    applyNow();
}

int MixerRouter::getReturnBus (const juce::String& insertID) const
{
    auto it = returnBuses.find (insertID);
    return it != returnBuses.end() ? it->second : -1;
}

//==============================================================================
void MixerRouter::computeSilenced()
{
    silenced.clearQuick();

    juce::StringArray insertIDs, soloInserts, soloChannels;

    for (auto* t : song::getInserts (edit))
    {
        const auto id = t->itemID.toString();
        insertIDs.add (id);

        if (solo.inserts.contains (id))
            soloInserts.add (id);
    }

    auto channels = song::getChannels (edit);

    for (auto* t : channels)
        if (solo.channels.contains (song::getChannelID (*t)))
            soloChannels.add (song::getChannelID (*t));

    if (soloInserts.isEmpty() && soloChannels.isEmpty())
        return;

    // Soloed inserts: they, what flows into them and what they flow into stay audible (like FL).
    juce::StringArray audibleInserts;

    if (soloInserts.isEmpty())
    {
        audibleInserts = insertIDs;
    }
    else
    {
        for (auto& id : insertIDs)
            for (auto& s : soloInserts)
                if (id == s || song::feeds (edit, id, s) || song::feeds (edit, s, id))
                    audibleInserts.addIfNotAlreadyThere (id);

        for (auto& id : insertIDs)
            if (! audibleInserts.contains (id))
                silenced.add (id);
    }

    for (auto* t : channels)
    {
        const auto id = song::getChannelID (*t);
        bool audible = soloChannels.isEmpty() || soloChannels.contains (id);

        if (audible && ! soloInserts.isEmpty())
        {
            auto* dest = song::resolveRoute (edit, *t);
            audible = dest != nullptr && audibleInserts.contains (dest->itemID.toString());
        }

        if (! audible)
            silenced.add (id);
    }
}

void MixerRouter::applyNow()
{
    cancelPendingUpdate();
    const juce::ScopedValueSetter<bool> svs (applying, true);

    computeSilenced();

    for (auto* track : te::getAudioTracks (edit))
    {
        auto& output = track->getOutput();

        if (silenced.contains (track->itemID.toString()))
        {
            if (output.getOutputName() != "(none)")
                output.setOutputToNone();   // soloed away: no sound on this PC

            continue;
        }

        auto* wanted = song::resolveRoute (edit, *track);

        if (wanted != nullptr)
        {
            if (output.getDestinationTrack() != wanted)
                output.setOutputToTrack (wanted);
        }
        else if (output.getDestinationTrack() != nullptr || ! output.usesDefaultAudioOut())
        {
            output.setOutputToDefaultDevice (false);
        }
    }

    applySends();
}

//==============================================================================
static te::Plugin::Ptr makeGenerated (te::Edit& edit, const juce::String& type, int bus)
{
    auto plugin = edit.getPluginCache().createNewPlugin (type, {});

    if (plugin != nullptr)
    {
        // Marked before it goes into the song, so it is never sent to anyone.
        plugin->state.setProperty (song::ids::generated, true, nullptr);
        plugin->state.setProperty (te::IDs::busNum, bus, nullptr);
    }

    return plugin;
}

static void removeGenerated (te::Plugin& p)
{
    auto parent = p.state.getParent();

    if (parent.isValid())
        parent.removeChild (p.state, nullptr);   // not part of anyone's undo history
}

void MixerRouter::applySends()
{
    auto inserts = song::getInserts (edit);
    juce::StringArray order;

    for (auto* t : inserts)
        order.add (t->itemID.toString());

    // The routes as the engine plays them, then each send that doesn't make a loop
    // (someone else may have made one at the same moment).
    std::map<juce::String, juce::StringArray> flowsTo;

    for (auto* t : inserts)
        if (auto* dest = song::resolveRoute (edit, *t))
            flowsTo[t->itemID.toString()].add (dest->itemID.toString());

    auto reaches = [&flowsTo] (const juce::String& from, const juce::String& to)
    {
        juce::StringArray toVisit { from }, visited;

        while (! toVisit.isEmpty())
        {
            auto id = toVisit[toVisit.size() - 1];
            toVisit.remove (toVisit.size() - 1);

            if (id == to)
                return true;

            if (visited.contains (id))
                continue;

            visited.add (id);
            toVisit.addArray (flowsTo[id]);
        }

        return false;
    };

    struct Wanted { juce::String from, to; float faderPos; };
    std::vector<Wanted> sends;

    for (auto s : song::getSends (edit))
    {
        const auto from = s[song::ids::from].toString(), to = s[song::ids::to].toString();

        if (from == to || ! order.contains (from) || ! order.contains (to) || reaches (to, from))
            continue;

        flowsTo[from].add (to);

        if (silenced.contains (from) || song::getSendLevel (s) <= song::minSendDb)
            continue;   // kept in the loop check, but nothing to send

        sends.push_back ({ from, to, te::decibelsToVolumeFaderPosition ((float) song::getSendLevel (s)) });
    }

    // One engine bus per insert that receives sends (the engine has 32).
    returnBuses.clear();

    for (auto& id : order)
        for (auto& w : sends)
            if (w.to == id && returnBuses.find (id) == returnBuses.end() && (int) returnBuses.size() < 32)
                returnBuses[id] = (int) returnBuses.size();

    for (auto* t : inserts)
    {
        const auto id = t->itemID.toString();
        const int wantedBus = getReturnBus (id);

        // The return: first in the chain, so the insert's effects process what it receives.
        bool haveReturn = false;

        for (auto* p : t->pluginList.getPlugins())
        {
            auto* ret = dynamic_cast<te::AuxReturnPlugin*> (p);

            if (ret == nullptr || ! song::isGeneratedPlugin (p))
                continue;

            if (wantedBus < 0 || haveReturn)
            {
                removeGenerated (*p);
                continue;
            }

            if (ret->busNumber.get() != wantedBus)
                p->state.setProperty (te::IDs::busNum, wantedBus, nullptr);

            haveReturn = true;
        }

        if (wantedBus >= 0 && ! haveReturn)
        {
            if (auto ret = makeGenerated (edit, te::AuxReturnPlugin::xmlTypeName, wantedBus))
            {
                int index = 0;

                while (index < t->state.getNumChildren() && ! t->state.getChild (index).hasType (te::IDs::PLUGIN))
                    ++index;

                t->state.addChild (ret->state, index, nullptr);
            }
        }

        // The sends: last in the chain (after the fader, like FL).
        std::vector<std::pair<int, float>> wantedSends;

        for (auto& w : sends)
            if (w.from == id)
                if (auto bus = getReturnBus (w.to); bus >= 0)
                    wantedSends.push_back ({ bus, w.faderPos });

        juce::Array<int> busesDone;

        for (auto* p : t->pluginList.getPlugins())
        {
            auto* send = dynamic_cast<te::AuxSendPlugin*> (p);

            if (send == nullptr || ! song::isGeneratedPlugin (p))
                continue;

            auto match = std::find_if (wantedSends.begin(), wantedSends.end(),
                                       [send] (auto& w) { return w.first == send->getBusNumber(); });

            if (match == wantedSends.end() || busesDone.contains (match->first))
            {
                removeGenerated (*p);
                continue;
            }

            // Through the parameter (the engine doesn't take it from the stored value); it
            // writes the value back without any undo history, and nothing here is ever sent.
            if (std::abs (send->gain->getCurrentValue() - match->second) > 1.0e-5f)
                send->gain->setParameter (match->second, juce::dontSendNotification);

            busesDone.add (match->first);
        }

        for (auto& [bus, pos] : wantedSends)
        {
            if (busesDone.contains (bus))
                continue;

            if (auto send = makeGenerated (edit, te::AuxSendPlugin::xmlTypeName, bus))
            {
                t->state.addChild (send->state, -1, nullptr);

                if (auto* aux = dynamic_cast<te::AuxSendPlugin*> (send.get()))
                    aux->gain->setParameter (pos, juce::dontSendNotification);

                busesDone.add (bus);
            }
        }
    }
}

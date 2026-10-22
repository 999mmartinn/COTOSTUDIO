#include "EditSync.h"

namespace collab
{

//==============================================================================
// What is NOT shared between peers.
//
// Node types: anything that describes this machine rather than the song.
static const juce::Identifier localNodeTypes[] =
{
    ids::SYNCLOCAL,                 // our own per-peer bookkeeping (ID range)
    "INPUTDEVICES",                 // which audio/MIDI inputs this PC has
    "VIEWSTATE",
    "TRANSPORT",                    // playhead, loop markers etc. (transport is synced separately)
};

// Properties that are per-machine wherever they appear.
static const juce::Identifier localProperties[] =
{
    "windowX", "windowY", "windowLocked", "windowState",
    "lastSignificantChange", "modifiedBy", "mtime",
};

// Properties that ARE sent, but are left out of the divergence hash because two
// peers can legitimately hold different bytes for the same sound (a plugin's
// saved chunk can contain UI sizes, timestamps...). Knob values are synced live.
static const juce::Identifier hashExcludedProperties[] =
{
    "state",
};

static const juce::Identifier stateProperty ("state");
static const juce::Identifier generatedProperty ("generated");

bool EditSync::isLocalNode (const juce::ValueTree& v)
{
    for (auto& t : localNodeTypes)
        if (v.hasType (t))
            return true;

    // Clips generated on each PC from the patterns (see PatternCompiler).
    return (bool) v[generatedProperty];
}

bool EditSync::isInsideLocal (const juce::ValueTree& v) const
{
    for (auto t = v; t.isValid() && t != root; t = t.getParent())
        if (isLocalNode (t))
            return true;

    return false;
}

bool EditSync::isLocalProperty (const juce::ValueTree&, const juce::Identifier& prop, bool forHash)
{
    if (prop == ids::stub)
        return true;

    for (auto& p : localProperties)
        if (prop == p)
            return true;

    if (forHash)
        for (auto& p : hashExcludedProperties)
            if (prop == p)
                return true;

    return false;
}

juce::ValueTree EditSync::filteredCopy (const juce::ValueTree& v, bool forHash)
{
    juce::ValueTree copy (v.getType());

    for (int i = 0; i < v.getNumProperties(); ++i)
    {
        auto name = v.getPropertyName (i);

        if (! isLocalProperty (v, name, forHash))
            copy.setProperty (name, v.getProperty (name), nullptr);
    }

    for (const auto& child : v)
        if (! isLocalNode (child))
            copy.appendChild (filteredCopy (child, forHash), nullptr);

    // The order of notes inside a pattern (ours: PATTERN, the engine's: SEQUENCE) doesn't change the music, and two
    // people adding notes at the same moment can end up with them in a different
    // order: don't count that as a difference.
    if (forHash && (v.hasType (te::IDs::SEQUENCE) || v.hasType ("PATTERN")))
    {
        struct ByKey
        {
            static int compareElements (const juce::ValueTree& a, const juce::ValueTree& b)
            {
                return (a.getType().toString() + stableKeyOf (a) + a.toXmlString())
                         .compare (b.getType().toString() + stableKeyOf (b) + b.toXmlString());
            }
        };

        ByKey sorter;
        copy.sort (sorter, nullptr, true);
    }

    return copy;
}

juce::String EditSync::describeFirstDifference (const juce::ValueTree& a, const juce::ValueTree& b)
{
    auto la = juce::StringArray::fromLines (filteredCopy (a, true).toXmlString());
    auto lb = juce::StringArray::fromLines (filteredCopy (b, true).toXmlString());

    for (int i = 0; i < juce::jmax (la.size(), lb.size()); ++i)
        if (la[i] != lb[i])
            return "line " + juce::String (i + 1) + "\n  mine:  " + la[i].trim().substring (0, 160)
                                                 + "\n  other: " + lb[i].trim().substring (0, 160);

    return "no difference";
}

//==============================================================================
EditSync::EditSync (te::Edit& e)
    : edit (e), root (e.state)
{
    trace = juce::SystemStats::getEnvironmentVariable ("SYNCTEST_TRACE", {}).isNotEmpty();
    root.addListener (this);
    refreshParameterListeners();
    startTimer (30);
}

EditSync::~EditSync()
{
    stopTimer();
    cancelPendingUpdate();
    root.removeListener (this);
    removeAllParameterListeners();
}

void EditSync::log (const juce::String& s)
{
    if (onLog)
        onLog (s);
}

void EditSync::send (const juce::ValueTree& op)
{
    ++stats.sent;

    auto kind = op[ids::kind].toString();
    auto desc = kind == "param" ? kind : kind + " " + resolve (op[ids::path].toString()).getType().toString();
    ++sentKinds[desc];

    if (trace)
        log ("SEND " + desc + " " + op[ids::path].toString()
              + (op.getNumChildren() > 0 ? "  " + op.getChild (0).toXmlString().replace ("\n", " ").substring (0, 300)
                                         : juce::String()));

    if (onLocalOp)
        onLocalOp (op);
}

//==============================================================================
// Addressing: "TRACK:1013/MIDICLIP:1020/SEQUENCE#0/NOTE#3"
// Children with an ID are named by it; others by their position among
// siblings of the same type that also have no ID.

static int actualIndexForSynced (const juce::ValueTree& parent, int syncedIndex)
{
    int n = 0;

    for (int i = 0; i < parent.getNumChildren(); ++i)
    {
        if (EditSync::isLocalNode (parent.getChild (i)))
            continue;

        if (n == syncedIndex)
            return i;

        ++n;
    }

    return parent.getNumChildren();
}

bool EditSync::needsSyncID (const juce::ValueTree& child)
{
    static const juce::Identifier listItems[] = { "NOTE", "CONTROL", "SYSEX", "PATTERN", "PNOTE" };

    if (child.hasProperty (te::IDs::id) || child.hasProperty (ids::sid))
        return false;

    for (auto& t : listItems)
        if (child.hasType (t))
            return true;

    return false;
}

juce::String EditSync::stableKeyOf (const juce::ValueTree& child)
{
    if (child.hasProperty (te::IDs::id))
        return child.getType().toString() + ":" + child[te::IDs::id].toString();

    if (child.hasProperty (ids::sid))
        return child.getType().toString() + "~" + child[ids::sid].toString();

    return {};
}

juce::String EditSync::keyOf (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    auto stable = stableKeyOf (child);

    if (stable.isNotEmpty())
        return stable;

    int n = 0;

    for (const auto& c : parent)
    {
        if (c == child)
            break;

        if (c.hasType (child.getType()) && stableKeyOf (c).isEmpty() && ! isLocalNode (c))
            ++n;
    }

    return child.getType().toString() + "#" + juce::String (n);
}

juce::String EditSync::pathOf (const juce::ValueTree& node) const
{
    juce::StringArray keys;

    for (auto v = node; v != root; )
    {
        auto parent = v.getParent();

        if (! parent.isValid() || isLocalNode (v))
            return "!";

        keys.insert (0, keyOf (parent, v));
        v = parent;
    }

    return keys.joinIntoString ("/");
}

bool EditSync::isAttachedAndShared (const juce::ValueTree& v) const
{
    return pathOf (v) != "!";
}

int EditSync::depthOf (const juce::String& path)
{
    return path.isEmpty() ? 0 : juce::StringArray::fromTokens (path, "/", {}).size();
}

juce::ValueTree EditSync::resolve (const juce::String& path) const
{
    auto v = root;

    for (auto& key : juce::StringArray::fromTokens (path, "/", {}))
    {
        if (key.isEmpty())
            continue;

        juce::ValueTree next;

        if (key.contains (":") || key.contains ("~"))
        {
            for (const auto& c : v)
                if (! isLocalNode (c) && stableKeyOf (c) == key)
                    { next = c; break; }
        }
        else
        {
            auto type = key.upToFirstOccurrenceOf ("#", false, false);
            int wanted = key.fromFirstOccurrenceOf ("#", false, false).getIntValue();
            int n = 0;

            for (const auto& c : v)
            {
                if (c.getType().toString() == type && stableKeyOf (c).isEmpty() && ! isLocalNode (c))
                {
                    if (n == wanted)
                        { next = c; break; }

                    ++n;
                }
            }
        }

        if (! next.isValid())
            return {};

        v = next;
    }

    return v;
}

//==============================================================================
// Collecting local changes

void EditSync::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& prop)
{
    if (applyingDepth > 0 || isLocalProperty (v, prop, false) || isInsideLocal (v))
        return;

    if (prop == stateProperty)
    {
        if (ScopedPluginStateCommit::depth > 0)
        {
            stateDirty.addIfNotAlreadyThere (v);
            triggerAsyncUpdate();
        }

        return;
    }

    propsDirty.addIfNotAlreadyThere (v);
    triggerAsyncUpdate();
}

void EditSync::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (applyingDepth > 0 || isLocalNode (child) || isInsideLocal (parent))
        return;

    if (needsSyncID (child))
        child.setProperty (ids::sid, juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()), nullptr);

    childrenDirty.addIfNotAlreadyThere (parent);
    addedChildren.addIfNotAlreadyThere (child);
    triggerAsyncUpdate();
}

void EditSync::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (applyingDepth > 0 || isLocalNode (child) || isInsideLocal (parent))
        return;

    addedChildren.removeAllInstancesOf (child);
    auto key = stableKeyOf (child);

    if (key.isNotEmpty())
    {
        auto it = std::find_if (removedKeys.begin(), removedKeys.end(), [&] (auto& p) { return p.first == parent; });

        if (it == removedKeys.end())
            removedKeys.push_back ({ parent, juce::StringArray (key) });
        else
            it->second.addIfNotAlreadyThere (key);
    }

    childrenDirty.addIfNotAlreadyThere (parent);
    triggerAsyncUpdate();
}

void EditSync::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (applyingDepth > 0 || isInsideLocal (parent))
        return;

    childrenDirty.addIfNotAlreadyThere (parent);
    triggerAsyncUpdate();
}

void EditSync::handleAsyncUpdate()
{
    flushNow();
}

juce::ValueTree EditSync::describeChildren (const juce::ValueTree& node) const
{
    juce::ValueTree desc (node.getType());

    for (const auto& child : node)
    {
        if (isLocalNode (child))
            continue;

        if (stableKeyOf (child).isNotEmpty() && ! addedChildren.contains (child))
        {
            // The receiver already has this one: name it, don't resend it.
            juce::ValueTree stub (child.getType());

            if (child.hasProperty (te::IDs::id))
                stub.setProperty (te::IDs::id, child[te::IDs::id], nullptr);
            else
                stub.setProperty (ids::sid, child[ids::sid], nullptr);

            stub.setProperty (ids::stub, true, nullptr);
            desc.appendChild (stub, nullptr);
        }
        else
        {
            desc.appendChild (filteredCopy (child, false), nullptr);
        }
    }

    for (auto& [parent, keys] : removedKeys)
        if (parent == node)
            desc.setProperty (ids::removedKeys, keys.joinIntoString ("|"), nullptr);

    return desc;
}

juce::ValueTree EditSync::describeProps (const juce::ValueTree& node) const
{
    juce::ValueTree desc (node.getType());

    for (int i = 0; i < node.getNumProperties(); ++i)
    {
        auto name = node.getPropertyName (i);

        if (! isLocalProperty (node, name, false) && name != stateProperty)
            desc.setProperty (name, node.getProperty (name), nullptr);
    }

    return desc;
}

void EditSync::flushNow()
{
    cancelPendingUpdate();

    if (propsDirty.isEmpty() && childrenDirty.isEmpty() && stateDirty.isEmpty())
        return;

    struct Pending { juce::String path; int depth; juce::ValueTree op; };
    std::vector<Pending> childOps, propOps;

    for (auto& node : childrenDirty)
    {
        auto path = pathOf (node);

        if (path == "!")
            continue;

        juce::ValueTree op (ids::OP);
        op.setProperty (ids::kind, "children", nullptr);
        op.setProperty (ids::path, path, nullptr);
        op.appendChild (describeChildren (node), nullptr);
        childOps.push_back ({ path, depthOf (path), op });
    }

    // A node that travelled in full inside a "children" description doesn't
    // need its own "props" op.
    auto isCovered = [this] (const juce::ValueTree& node)
    {
        for (auto x = node; x.isValid() && x != root; x = x.getParent())
        {
            auto parent = x.getParent();

            if (childrenDirty.contains (parent) && (addedChildren.contains (x) || stableKeyOf (x).isEmpty()))
                return true;
        }

        return false;
    };

    for (auto& node : propsDirty)
    {
        auto path = pathOf (node);

        if (path == "!" || isCovered (node))
            continue;

        juce::ValueTree op (ids::OP);
        op.setProperty (ids::kind, "props", nullptr);
        op.setProperty (ids::path, path, nullptr);
        op.appendChild (describeProps (node), nullptr);
        propOps.push_back ({ path, depthOf (path), op });
    }

    // Parents before children so every path exists when it is applied.
    auto byDepth = [] (const Pending& a, const Pending& b) { return a.depth < b.depth; };
    std::stable_sort (childOps.begin(), childOps.end(), byDepth);
    std::stable_sort (propOps.begin(), propOps.end(), byDepth);

    std::vector<juce::ValueTree> stateOps;

    for (auto& node : stateDirty)
    {
        auto path = pathOf (node);

        if (path == "!" || ! node.hasProperty (stateProperty))
            continue;

        juce::ValueTree op (ids::OP);
        op.setProperty (ids::kind, "pluginState", nullptr);
        op.setProperty (ids::path, path, nullptr);
        op.setProperty (ids::value, node[stateProperty], nullptr);
        stateOps.push_back (op);
    }

    propsDirty.clear();
    childrenDirty.clear();
    addedChildren.clear();
    removedKeys.clear();
    stateDirty.clear();

    for (auto& p : childOps)  send (p.op);
    for (auto& p : propOps)   send (p.op);
    for (auto& op : stateOps) send (op);
}

//==============================================================================
// Applying remote changes

bool EditSync::reconcileProps (juce::ValueTree& local, const juce::ValueTree& incoming)
{
    for (int i = 0; i < incoming.getNumProperties(); ++i)
    {
        auto name = incoming.getPropertyName (i);

        if (isLocalProperty (local, name, false))
            continue;

        auto value = incoming.getProperty (name);

        if (! local.hasProperty (name) || local.getProperty (name) != value)
        {
            local.setProperty (name, value, nullptr);
            restoreExternalPluginStateIfNeeded (local, name);
        }
    }

    for (int i = local.getNumProperties(); --i >= 0;)
    {
        auto name = local.getPropertyName (i);

        if (! isLocalProperty (local, name, false) && name != stateProperty && ! incoming.hasProperty (name))
            local.removeProperty (name, nullptr);
    }

    // Built-in plugins: the engine updates the stored value but leaves the live
    // parameter alone ("always go via setParameter"), so push it through here.
    if (local.hasType (te::IDs::PLUGIN))
        if (auto plugin = te::findPluginForID (edit, te::EditItemID::fromVar (local[te::IDs::id])))
            if (dynamic_cast<te::ExternalPlugin*> (plugin.get()) == nullptr)
                for (auto* ap : plugin->getAutomatableParameters())
                    ap->updateFromAttachedValue();

    return true;
}

bool EditSync::reconcileNode (juce::ValueTree& local, const juce::ValueTree& incoming)
{
    bool ok = reconcileProps (local, incoming);
    return reconcileChildren (local, incoming) && ok;
}

bool EditSync::reconcileChildren (juce::ValueTree& local, const juce::ValueTree& incoming)
{
    bool ok = true;

    juce::Array<juce::ValueTree> existing;

    for (const auto& c : local)
        if (! isLocalNode (c))
            existing.add (c);

    const auto removed = juce::StringArray::fromTokens (incoming[ids::removedKeys].toString(), "|", {});
    juce::Array<juce::ValueTree> desired;
    std::map<juce::String, int> occurrence;

    for (const auto& inc : incoming)
    {
        juce::ValueTree match;
        const auto key = stableKeyOf (inc);

        if (key.isNotEmpty())
        {
            for (auto& c : existing)
                if (stableKeyOf (c) == key && ! desired.contains (c))
                    { match = c; break; }
        }
        else
        {
            const auto type = inc.getType();
            const int wanted = occurrence[type.toString()]++;
            int n = 0;

            for (auto& c : existing)
            {
                if (c.hasType (type) && stableKeyOf (c).isEmpty())
                {
                    if (n == wanted)
                        { match = c; break; }

                    ++n;
                }
            }
        }

        if (match.isValid())
        {
            if (! (bool) inc[ids::stub])
                ok = reconcileNode (match, inc) && ok;

            desired.add (match);
        }
        else if ((bool) inc[ids::stub])
        {
            ok = false;     // the sender thinks we have it, but we don't
        }
        else
        {
            desired.add (inc.createCopy());
        }
    }

    // Children the sender didn't mention:
    //  - with a stable key: someone else probably added them at the same time,
    //    so keep them unless the sender explicitly removed them;
    //  - without one: the sender's list is the truth, remove them.
    for (auto& c : existing)
    {
        if (desired.contains (c))
            continue;

        const auto key = stableKeyOf (c);

        if (key.isEmpty() || removed.contains (key))
            local.removeChild (c, nullptr);
        else
            desired.add (c);
    }

    for (int i = 0; i < desired.size(); ++i)
    {
        auto child = desired.getReference (i);
        const int target = actualIndexForSynced (local, i);
        const int current = local.indexOf (child);

        if (current < 0)
            local.addChild (child, target, nullptr);
        else if (current != target && target < local.getNumChildren())
            local.moveChild (current, target, nullptr);
    }

    return ok;
}

bool EditSync::applyRemote (const juce::ValueTree& op)
{
    const auto kind = op[ids::kind].toString();

    if (kind == "param")
    {
        auto pluginID = op[ids::plugin].toString();
        auto paramID = op[ids::param].toString();
        auto value = (float) op[ids::value];

        if (auto plugin = te::findPluginForID (edit, te::EditItemID::fromVar (pluginID)))
        {
            if (auto ap = plugin->getAutomatableParameterByID (paramID))
            {
                lastRemoteParam[pluginID + "|" + paramID] = value;
                ScopedApplying sa (*this);
                ap->setParameter (value, juce::sendNotification);
                ++stats.paramsApplied;
                return true;
            }
        }

        // The plugin may simply not be installed on this PC: not a divergence.
        return true;
    }

    // Anything we had collected but not yet sent describes an older state of
    // the nodes we are about to change; send it first so ordering is kept.
    flushNow();

    ScopedApplying sa (*this);
    auto target = resolve (op[ids::path].toString());

    if (! target.isValid() || (kind != "pluginState" && op.getNumChildren() != 1))
    {
        ++stats.failed;
        log ("Could not find '" + op[ids::path].toString() + "' for a '" + kind + "' change");
        return false;
    }

    bool ok = false;

    if (kind == "pluginState")
    {
        if (target.getProperty (stateProperty) != op[ids::value])
        {
            target.setProperty (stateProperty, op[ids::value], nullptr);
            restoreExternalPluginStateIfNeeded (target, stateProperty);
        }

        ++stats.applied;
        return true;
    }

    if (kind == "props")
        ok = reconcileProps (target, op.getChild (0));
    else if (kind == "children")
        ok = reconcileChildren (target, op.getChild (0));

    if (ok)
        ++stats.applied;
    else
    {
        ++stats.failed;
        log ("Could not fully apply a '" + kind + "' change at '" + op[ids::path].toString() + "'");
    }

    return ok;
}

void EditSync::restoreExternalPluginStateIfNeeded (const juce::ValueTree& node, const juce::Identifier& prop)
{
    // A third-party plugin's full state (preset, internal settings) arrives as the
    // "state" property. Tracktion doesn't reload it by itself, so do it here.
    if (prop != stateProperty || ! node.hasType (te::IDs::PLUGIN))
        return;

    if (auto plugin = te::findPluginForID (edit, te::EditItemID::fromVar (node[te::IDs::id])))
        if (auto ext = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
            ext->restorePluginStateFromValueTree (node);
}

//==============================================================================
// Live knobs of third-party plugins

void EditSync::parameterChanged (te::AutomatableParameter& param, float newValue)
{
    if (trace)
        log ("knob " + param.paramID + " = " + juce::String (newValue) + (applyingDepth > 0 ? " (remote)" : ""));

    if (applyingDepth > 0)
        return;

    auto* plugin = param.getPlugin();

    if (plugin == nullptr)
        return;

    auto pluginID = plugin->itemID.toString();
    auto key = pluginID + "|" + param.paramID;

    // Ignore the plugin echoing back a value we just received.
    if (auto it = lastRemoteParam.find (key); it != lastRemoteParam.end())
    {
        if (std::abs (it->second - newValue) < 1.0e-6f)
            return;

        lastRemoteParam.erase (it);
    }

    pendingParams[key] = { pluginID, param.paramID, newValue };
}

void EditSync::flushPendingParams()
{
    for (auto& [key, p] : pendingParams)
    {
        juce::ValueTree op (ids::OP);
        op.setProperty (ids::kind, "param", nullptr);
        op.setProperty (ids::plugin, p.pluginID, nullptr);
        op.setProperty (ids::param, p.paramID, nullptr);
        op.setProperty (ids::value, p.value, nullptr);
        ++stats.paramsSent;
        send (op);
    }

    pendingParams.clear();
}

void EditSync::refreshParameterListeners()
{
    // Only third-party plugins: built-in plugins store their knobs in the tree,
    // which is already synced, so listening to them too would send twice.
    std::vector<te::AutomatableParameter::Ptr> current;

    for (auto plugin : te::getAllPlugins (edit, false))
        if (dynamic_cast<te::ExternalPlugin*> (plugin) != nullptr)
            for (auto* ap : plugin->getAutomatableParameters())
                current.push_back (ap);

    std::set<te::AutomatableParameter*> currentSet;

    for (auto& p : current)
        currentSet.insert (p.get());

    if (currentSet == listenedSet)
        return;

    for (auto& p : listenedParams)
        if (currentSet.count (p.get()) == 0)
            p->removeListener (this);

    for (auto& p : current)
        if (listenedSet.count (p.get()) == 0)
            p->addListener (this);

    if (trace)
        log ("listening to " + juce::String ((int) current.size()) + " plugin parameters");

    listenedParams = std::move (current);
    listenedSet = std::move (currentSet);
}

void EditSync::removeAllParameterListeners()
{
    for (auto& p : listenedParams)
        p->removeListener (this);

    listenedParams.clear();
    listenedSet.clear();
}

void EditSync::timerCallback()
{
    flushPendingParams();

    // Plugin instances (and so their parameters) are created asynchronously,
    // so re-check which parameters exist a couple of times per second.
    if (++tick % 5 == 0)
        refreshParameterListeners();
}

//==============================================================================
juce::StringArray EditSync::getTopSentKinds (int maxItems) const
{
    std::vector<std::pair<int, juce::String>> sorted;

    for (auto& [k, n] : sentKinds)
        sorted.push_back ({ n, k });

    std::sort (sorted.begin(), sorted.end(), [] (auto& a, auto& b) { return a.first > b.first; });

    juce::StringArray out;

    for (size_t i = 0; i < sorted.size() && (int) i < maxItems; ++i)
        out.add (juce::String (sorted[i].first) + "x  " + sorted[i].second);

    return out;
}

void EditSync::assignMissingSyncIDs()
{
    std::function<void (juce::ValueTree)> visit = [&] (juce::ValueTree v)
    {
        for (auto c : v)
        {
            if (needsSyncID (c))
                c.setProperty (ids::sid, juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()), nullptr);

            visit (c);
        }
    };

    visit (root);
}

juce::int64 EditSync::computeHash() const
{
    return filteredCopy (root, true).toXmlString().hashCode64();
}

juce::ValueTree EditSync::createSnapshot()
{
    // Write every plugin's current state into the tree. These writes are sent
    // to the other peers too, so everyone keeps the same copy.
    edit.flushState();
    flushNow();

    return filteredCopy (root, false);
}

} // namespace collab

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

bool EditSync::isLocalNode (const juce::ValueTree& v)
{
    for (auto& t : localNodeTypes)
        if (v.hasType (t))
            return true;

    return false;
}

bool EditSync::isLocalProperty (const juce::ValueTree&, const juce::Identifier& prop, bool forHash)
{
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
    root.addListener (this);
    refreshParameterListeners();
    startTimer (30);
}

EditSync::~EditSync()
{
    stopTimer();
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

    if (onLocalOp)
        onLocalOp (op);
}

//==============================================================================
int EditSync::syncedIndexOf (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    int n = 0;

    for (const auto& c : parent)
    {
        if (c == child)
            return n;

        if (! isLocalNode (c))
            ++n;
    }

    return -1;
}

int EditSync::syncedIndexBefore (const juce::ValueTree& parent, int actualIndex)
{
    int n = 0;

    for (int i = 0; i < juce::jmin (actualIndex, parent.getNumChildren()); ++i)
        if (! isLocalNode (parent.getChild (i)))
            ++n;

    return n;
}

int EditSync::actualIndexForSynced (const juce::ValueTree& parent, int syncedIndex)
{
    int n = 0;

    for (int i = 0; i < parent.getNumChildren(); ++i)
    {
        if (isLocalNode (parent.getChild (i)))
            continue;

        if (n == syncedIndex)
            return i;

        ++n;
    }

    return syncedIndex == n ? parent.getNumChildren() : -1;
}

bool EditSync::isInsideLocal (const juce::ValueTree& v) const
{
    for (auto n = v; n.isValid() && n != root; n = n.getParent())
        if (isLocalNode (n))
            return true;

    return false;
}

juce::String EditSync::pathOf (const juce::ValueTree& node) const
{
    juce::StringArray parts;

    for (auto v = node; v != root; )
    {
        auto parent = v.getParent();

        if (! parent.isValid())
            return "!";     // not attached to this Edit

        parts.insert (0, juce::String (syncedIndexOf (parent, v)));
        v = parent;
    }

    return parts.joinIntoString ("/");
}

juce::ValueTree EditSync::resolve (const juce::String& path) const
{
    auto v = root;

    for (auto& token : juce::StringArray::fromTokens (path, "/", {}))
    {
        if (token.isEmpty())
            continue;

        auto i = actualIndexForSynced (v, token.getIntValue());

        if (i < 0 || i >= v.getNumChildren())
            return {};

        v = v.getChild (i);
    }

    return v;
}

//==============================================================================
void EditSync::valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& prop)
{
    if (applyingDepth > 0 || isInsideLocal (v) || isLocalProperty (v, prop, false))
        return;

    auto path = pathOf (v);

    if (path == "!")
        return;

    juce::ValueTree op (ids::OP);
    op.setProperty (ids::kind, "prop", nullptr);
    op.setProperty (ids::path, path, nullptr);
    op.setProperty (ids::name, prop.toString(), nullptr);

    if (v.hasProperty (prop))
        op.setProperty (ids::value, v.getProperty (prop), nullptr);
    else
        op.setProperty (ids::removed, true, nullptr);

    send (op);
}

void EditSync::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (applyingDepth > 0 || isLocalNode (child) || isInsideLocal (parent))
        return;

    auto path = pathOf (parent);

    if (path == "!")
        return;

    juce::ValueTree op (ids::OP);
    op.setProperty (ids::kind, "add", nullptr);
    op.setProperty (ids::path, path, nullptr);
    op.setProperty (ids::index, syncedIndexOf (parent, child), nullptr);
    op.appendChild (filteredCopy (child, false), nullptr);
    send (op);
}

void EditSync::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index)
{
    if (applyingDepth > 0 || isLocalNode (child) || isInsideLocal (parent))
        return;

    auto path = pathOf (parent);

    if (path == "!")
        return;

    juce::ValueTree op (ids::OP);
    op.setProperty (ids::kind, "remove", nullptr);
    op.setProperty (ids::path, path, nullptr);
    op.setProperty (ids::index, syncedIndexBefore (parent, index), nullptr);
    op.setProperty (ids::childType, child.getType().toString(), nullptr);
    send (op);
}

void EditSync::valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex)
{
    if (applyingDepth > 0 || isInsideLocal (parent))
        return;

    auto moved = parent.getChild (newIndex);

    if (isLocalNode (moved))
        return;

    // Rebuild the order before the move to know the moved child's old synced index.
    std::vector<juce::ValueTree> before;

    for (const auto& c : parent)
        before.push_back (c);

    before.erase (before.begin() + newIndex);
    before.insert (before.begin() + oldIndex, moved);

    int oldSynced = 0;

    for (int i = 0; i < oldIndex; ++i)
        if (! isLocalNode (before[(size_t) i]))
            ++oldSynced;

    juce::ValueTree op (ids::OP);
    op.setProperty (ids::kind, "move", nullptr);
    op.setProperty (ids::path, pathOf (parent), nullptr);
    op.setProperty (ids::oldIndex, oldSynced, nullptr);
    op.setProperty (ids::newIndex, syncedIndexOf (parent, moved), nullptr);
    send (op);
}

//==============================================================================
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

    ScopedApplying sa (*this);
    auto target = resolve (op[ids::path].toString());

    if (! target.isValid())
    {
        ++stats.failed;
        log ("Could not find " + op[ids::path].toString() + " for a '" + kind + "' change");
        return false;
    }

    bool ok = true;

    if (kind == "prop")
    {
        juce::Identifier name (op[ids::name].toString());

        if (op[ids::removed])
            target.removeProperty (name, nullptr);
        else
            target.setProperty (name, op[ids::value], nullptr);

        restoreExternalPluginStateIfNeeded (target, name);
    }
    else if (kind == "add")
    {
        auto index = actualIndexForSynced (target, (int) op[ids::index]);

        if (index < 0 || op.getNumChildren() != 1)
            ok = false;
        else
            target.addChild (op.getChild (0).createCopy(), index, nullptr);
    }
    else if (kind == "remove")
    {
        auto index = actualIndexForSynced (target, (int) op[ids::index]);
        auto child = target.getChild (index);

        if (index < 0 || ! child.isValid() || child.getType().toString() != op[ids::childType].toString())
            ok = false;
        else
            target.removeChild (index, nullptr);
    }
    else if (kind == "move")
    {
        auto from = actualIndexForSynced (target, (int) op[ids::oldIndex]);
        auto to   = actualIndexForSynced (target, (int) op[ids::newIndex]);

        if (from < 0 || to < 0 || from >= target.getNumChildren() || to >= target.getNumChildren())
            ok = false;
        else
            target.moveChild (from, to, nullptr);
    }
    else
    {
        ok = false;
    }

    if (ok)
        ++stats.applied;
    else
    {
        ++stats.failed;
        log ("Could not apply a '" + kind + "' change at " + op[ids::path].toString());
    }

    return ok;
}

void EditSync::restoreExternalPluginStateIfNeeded (const juce::ValueTree& node, const juce::Identifier& prop)
{
    // A third-party plugin's full state (preset, internal settings) arrives as the
    // "state" property. Tracktion doesn't reload it by itself, so do it here.
    if (prop != juce::Identifier ("state") || ! node.hasType (te::IDs::PLUGIN))
        return;

    if (auto plugin = te::findPluginForID (edit, te::EditItemID::fromVar (node[te::IDs::id])))
        if (auto ext = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
            ext->restorePluginStateFromValueTree (node);
}

//==============================================================================
void EditSync::parameterChanged (te::AutomatableParameter& param, float newValue)
{
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
    if (++tick % 15 == 0)
        refreshParameterListeners();
}

//==============================================================================
juce::int64 EditSync::computeHash() const
{
    return filteredCopy (root, true).toXmlString().hashCode64();
}

juce::ValueTree EditSync::createSnapshot()
{
    {
        // Write every plugin's current state into the tree, without sending
        // those writes as ops (the snapshot carries them).
        ScopedApplying sa (*this);
        edit.flushState();
    }

    return filteredCopy (root, false);
}

} // namespace collab

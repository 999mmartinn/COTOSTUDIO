#pragma once

#include <JuceHeader.h>
#include <map>
#include <set>

namespace te = tracktion;

namespace collab
{

//==============================================================================
// Property / type names used by the sync protocol.
namespace ids
{
    #define SYNC_DECLARE_ID(name) inline const juce::Identifier name { #name };
    SYNC_DECLARE_ID (MSG)
    SYNC_DECLARE_ID (OP)
    SYNC_DECLARE_ID (SYNCLOCAL)
    SYNC_DECLARE_ID (type)
    SYNC_DECLARE_ID (kind)
    SYNC_DECLARE_ID (path)
    SYNC_DECLARE_ID (name)
    SYNC_DECLARE_ID (value)
    SYNC_DECLARE_ID (removed)
    SYNC_DECLARE_ID (index)
    SYNC_DECLARE_ID (oldIndex)
    SYNC_DECLARE_ID (newIndex)
    SYNC_DECLARE_ID (childType)
    SYNC_DECLARE_ID (plugin)
    SYNC_DECLARE_ID (param)
    SYNC_DECLARE_ID (seq)
    SYNC_DECLARE_ID (ack)
    SYNC_DECLARE_ID (hash)
    SYNC_DECLARE_ID (peer)
    SYNC_DECLARE_ID (peerName)
    SYNC_DECLARE_ID (protocol)
    SYNC_DECLARE_ID (playing)
    SYNC_DECLARE_ID (position)
    SYNC_DECLARE_ID (reason)
    SYNC_DECLARE_ID (id)
    #undef SYNC_DECLARE_ID
}

//==============================================================================
/**
    Mirrors one Tracktion Edit to and from other peers.

    Two channels:
      1. Structure: every change to the Edit's ValueTree (tracks, clips, notes,
         plugin inserts, built-in plugin settings) becomes a small "op" with a
         path made of child indices. Nodes and properties that are purely local
         (window positions, transport position...) are skipped.
      2. Live knobs: third-party plugins (VST3) don't write their knob values to
         the ValueTree while you turn them, so their AutomatableParameters are
         listened to directly and sent as "param" ops (coalesced every 30 ms).

    Remote ops are applied with notifications suppressed so they are not echoed.
    A hash of the shared part of the tree lets peers detect divergence.
*/
class EditSync  : private juce::ValueTree::Listener,
                  private te::AutomatableParameter::Listener,
                  private juce::Timer
{
public:
    explicit EditSync (te::Edit&);
    ~EditSync() override;

    te::Edit& getEdit() noexcept                         { return edit; }

    /** Called (message thread) for every local change that should be sent. */
    std::function<void (const juce::ValueTree& op)> onLocalOp;
    /** Called after one or more remote ops were applied (for UI refresh). */
    std::function<void()> onRemoteChange;
    std::function<void (const juce::String&)> onLog;

    /** Applies an op from another peer. Returns false if it could not be
        applied (the trees have diverged and a resync is needed). */
    bool applyRemote (const juce::ValueTree& op);

    /** Hash of the shared state (local-only data excluded). */
    juce::int64 computeHash() const;

    /** Flushes plugin states and returns a copy of the shared state. */
    juce::ValueTree createSnapshot();

    struct Stats { int sent = 0, applied = 0, failed = 0, paramsSent = 0, paramsApplied = 0; };
    Stats getStats() const noexcept                      { return stats; }

    //==============================================================================
    static bool isLocalNode (const juce::ValueTree&);
    static bool isLocalProperty (const juce::ValueTree& node, const juce::Identifier&, bool forHash);
    static juce::ValueTree filteredCopy (const juce::ValueTree&, bool forHash);
    static juce::String describeFirstDifference (const juce::ValueTree& a, const juce::ValueTree& b);

private:
    te::Edit& edit;
    juce::ValueTree root;
    int applyingDepth = 0;
    int tick = 0;
    Stats stats;

    struct ScopedApplying
    {
        explicit ScopedApplying (EditSync& s) : owner (s)   { ++owner.applyingDepth; }
        ~ScopedApplying()                                    { --owner.applyingDepth; }
        EditSync& owner;
    };

    // Parameter (knob) sync for external plugins
    std::vector<te::AutomatableParameter::Ptr> listenedParams;
    std::set<te::AutomatableParameter*> listenedSet;
    struct PendingParam { juce::String pluginID, paramID; float value; };
    std::map<juce::String, PendingParam> pendingParams;
    std::map<juce::String, float> lastRemoteParam;

    void refreshParameterListeners();
    void removeAllParameterListeners();
    void flushPendingParams();

    // Path helpers (indices count only non-local siblings)
    static int syncedIndexOf (const juce::ValueTree& parent, const juce::ValueTree& child);
    static int syncedIndexBefore (const juce::ValueTree& parent, int actualIndex);
    static int actualIndexForSynced (const juce::ValueTree& parent, int syncedIndex);
    bool isInsideLocal (const juce::ValueTree&) const;
    juce::String pathOf (const juce::ValueTree&) const;
    juce::ValueTree resolve (const juce::String& path) const;

    void send (const juce::ValueTree& op);
    void log (const juce::String&);
    void restoreExternalPluginStateIfNeeded (const juce::ValueTree& node, const juce::Identifier&);

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override;
    void valueTreeParentChanged (juce::ValueTree&) override {}

    // AutomatableParameter::Listener
    void curveHasChanged (te::AutomatableParameter&) override {}
    void parameterChanged (te::AutomatableParameter&, float newValue) override;

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditSync)
};

} // namespace collab

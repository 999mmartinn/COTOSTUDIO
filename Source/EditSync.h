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
    SYNC_DECLARE_ID (stub)
    SYNC_DECLARE_ID (sid)
    SYNC_DECLARE_ID (removedKeys)
    SYNC_DECLARE_ID (tree)
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
    SYNC_DECLARE_ID (pattern)
    SYNC_DECLARE_ID (patternOnly)
    SYNC_DECLARE_ID (id)
    #undef SYNC_DECLARE_ID
}

//==============================================================================
/**
    Mirrors one Tracktion Edit to and from other peers.

    Two channels:
      1. Structure. Changed parts of the Edit's ValueTree (tracks, clips, notes,
         plugin inserts, built-in plugin settings) are collected during one
         message-loop turn and then sent as *descriptions of how they look now*:
           - "props":    every shared property of one node;
           - "children": the ordered list of a node's children. Children with an
                         ID that the receiver already has are sent as stubs, new
                         ones and ID-less ones (notes...) are sent in full.
         The receiver reconciles its copy with that description, matching
         children by type + ID (or by position among siblings of the same type
         when there is no ID). Applying a description twice changes nothing, so
         the engine's own follow-up changes on both sides settle instead of
         echoing back and forth.
         Nodes are addressed by key paths like "TRACK:1013/MIDICLIP:1020/SEQUENCE#0".
      2. Live knobs. Third-party plugins (VST3) don't write knob values to the
         tree while you turn them, so their parameters are listened to directly
         and sent as "param" ops (coalesced every 30 ms).

    Local-only data (window positions, transport, input devices) is never sent.
    A hash of the shared state lets peers detect divergence.
*/
class EditSync  : private juce::ValueTree::Listener,
                  private te::AutomatableParameter::Listener,
                  private juce::Timer,
                  private juce::AsyncUpdater
{
public:
    explicit EditSync (te::Edit&);
    ~EditSync() override;

    te::Edit& getEdit() noexcept                         { return edit; }

    /** Called (message thread) for every local change that should be sent. */
    std::function<void (const juce::ValueTree& op)> onLocalOp;
    std::function<void (const juce::String&)> onLog;

    /** While one of these exists, a plugin state written into the song (e.g. when
        its window closes after loading a preset) is sent to the other peers.
        Other plugin-state writes (the engine saving in the background) are not
        sent: live knob values travel separately, and a late background save
        could otherwise overwrite a newer knob value on another PC. */
    struct ScopedPluginStateCommit
    {
        ScopedPluginStateCommit()   { ++depth; }
        ~ScopedPluginStateCommit()  { --depth; }
        static inline std::atomic<int> depth { 0 };
    };

    /** Applies an op from another peer. Returns false if it could not be
        applied (the copies have diverged and a fresh copy is needed). */
    bool applyRemote (const juce::ValueTree& op);

    /** Sends any collected changes now (normally done automatically). */
    void flushNow();

    /** Hash of the shared state (local-only data excluded). */
    juce::int64 computeHash() const;

    /** Gives notes (and other list items without an engine ID) a sync ID so
        two people can edit the same list at once. Call on the host's song. */
    void assignMissingSyncIDs();

    /** Flushes plugin states and returns a copy of the shared state. */
    juce::ValueTree createSnapshot();

    struct Stats { int sent = 0, applied = 0, failed = 0, paramsSent = 0, paramsApplied = 0; };
    Stats getStats() const noexcept                      { return stats; }

    /** The most frequently sent kinds of change, for diagnostics. */
    juce::StringArray getTopSentKinds (int maxItems) const;

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
    std::map<juce::String, int> sentKinds;
    bool trace = false;

    struct ScopedApplying
    {
        explicit ScopedApplying (EditSync& s) : owner (s)   { ++owner.applyingDepth; }
        ~ScopedApplying()                                    { --owner.applyingDepth; }
        EditSync& owner;
    };

    // Changes collected during this message-loop turn
    juce::Array<juce::ValueTree> propsDirty, childrenDirty, addedChildren, stateDirty;
    std::vector<std::pair<juce::ValueTree, juce::StringArray>> removedKeys;

    // Parameter (knob) sync for external plugins
    std::vector<te::AutomatableParameter::Ptr> listenedParams;
    std::set<te::AutomatableParameter*> listenedSet;
    struct PendingParam { juce::String pluginID, paramID; float value; };
    std::map<juce::String, PendingParam> pendingParams;
    std::map<juce::String, float> lastRemoteParam;

    void refreshParameterListeners();
    void removeAllParameterListeners();
    void flushPendingParams();

    // Addressing
    static juce::String keyOf (const juce::ValueTree& parent, const juce::ValueTree& child);
    static juce::String stableKeyOf (const juce::ValueTree& child);   // empty if positional
    static bool needsSyncID (const juce::ValueTree& child);
    bool isAttachedAndShared (const juce::ValueTree&) const;
    bool isInsideLocal (const juce::ValueTree&) const;
    juce::String pathOf (const juce::ValueTree&) const;
    juce::ValueTree resolve (const juce::String& path) const;
    static int depthOf (const juce::String& path);

    // Building and applying descriptions
    juce::ValueTree describeChildren (const juce::ValueTree& node) const;
    juce::ValueTree describeProps (const juce::ValueTree& node) const;
    bool reconcileProps (juce::ValueTree& local, const juce::ValueTree& incoming);
    bool reconcileChildren (juce::ValueTree& local, const juce::ValueTree& incoming);
    bool reconcileNode (juce::ValueTree& local, const juce::ValueTree& incoming);

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
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditSync)
};

} // namespace collab

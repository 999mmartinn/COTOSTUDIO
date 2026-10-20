#pragma once

#include "EditSync.h"

namespace collab
{

class Session;

//==============================================================================
/** One TCP link to another peer (length-prefixed binary ValueTree messages). */
class PeerConnection  : public juce::InterprocessConnection
{
public:
    explicit PeerConnection (Session& s);
    ~PeerConnection() override;

    void sendTree (const juce::ValueTree& msg);

    Session& session;
    int peerIndex = -1;          // assigned by the host
    juce::String peerName;
    int lastSeqApplied = 0;      // host side: last op batch received from this peer
    bool welcomed = false;
    bool dead = false;

private:
    void connectionMade() override;
    void connectionLost() override;
    void messageReceived (const juce::MemoryBlock&) override;
};

//==============================================================================
/**
    Owns the Edit being worked on and keeps it in sync with other peers.

    Star topology: one peer hosts, the others join. The host relays every
    change to everyone else, sends a hash of the shared state every 1.5 s, and
    sends a full snapshot to a peer whose hash keeps disagreeing.
*/
class Session  : private juce::Timer
{
public:
    explicit Session (te::Engine&);
    ~Session() override;

    enum class Mode { offline, hosting, connecting, joined };

    /** Starts with a local Edit (offline). */
    void createLocalEdit();

    bool startHosting (int port);
    bool join (const juce::String& hostAddress, int port);
    void leave();

    te::Edit* getEdit() const noexcept           { return edit.get(); }
    EditSync* getSync() const noexcept           { return editSync.get(); }
    Mode getMode() const noexcept                { return mode; }
    int getPeerIndex() const noexcept            { return peerIndex; }
    int getNumConnectedPeers() const;
    juce::String getStatusText() const;
    int getResyncCount() const noexcept          { return resyncCount; }
    bool isInSync() const noexcept               { return lastHashMatched; }

    /** Play/stop is shared: each PC renders its own audio from the same song. */
    void setPlaying (bool shouldPlay);

    std::function<void()> onEditAboutToBeReplaced;
    std::function<void()> onEditReplaced;
    std::function<void()> onRemoteChange;
    std::function<void()> onStatusChanged;
    std::function<void (const juce::String&)> onLog;

    juce::String peerName = juce::SystemStats::getComputerName();

    static constexpr int protocolVersion = 1;
    static constexpr juce::uint32 magicNumber = 0x53594e43; // "SYNC"
    static constexpr juce::int64 idRangePerPeer = 1000000000; // keeps new IDs from clashing

    //==============================================================================
    // Called by PeerConnection (message thread)
    void handleConnectionMade (PeerConnection&);
    void handleConnectionLost (PeerConnection&);
    void handleMessage (PeerConnection&, const juce::ValueTree&);

private:
    class Server;

    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<EditSync> editSync;
    std::unique_ptr<Server> server;
    juce::OwnedArray<PeerConnection, juce::CriticalSection> clients;   // host side (added from the server thread)
    std::unique_ptr<PeerConnection> hostLink;     // joining side
    Mode mode = Mode::offline;
    int peerIndex = 0;
    int nextPeerIndex = 1;
    int localSeq = 0;
    int hashMismatchCount = 0;
    int resyncCount = 0;
    bool lastHashMatched = true;
    juce::uint32 lastResyncRequestTime = 0;
    juce::ValueTree pendingOps { ids::MSG };

    void installEdit (std::unique_ptr<te::Edit>);
    void attachSync();
    void sendLocalOp (const juce::ValueTree& op);
    void flushOutgoing();
    void broadcast (const juce::ValueTree& msg, PeerConnection* except);
    void applyOps (const juce::ValueTree& msg, PeerConnection* from);
    void applyTransport (const juce::ValueTree& msg);
    void sendSnapshotTo (PeerConnection&, const juce::String& reason);
    void loadSnapshot (const juce::ValueTree& msg);
    void log (const juce::String&);
    void statusChanged();
    void timerCallback() override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Session)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};

//==============================================================================
/** Creates the starter song: one track with the built-in synth and a 1-bar pattern. */
void addStarterContent (te::Edit&);

} // namespace collab

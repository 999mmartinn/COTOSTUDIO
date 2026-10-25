#pragma once

#include "EditSync.h"
#include "AssetStore.h"
#include "Recorder.h"
#include <deque>

class PatternCompiler;
class MixerRouter;

namespace collab
{

class Session;

//==============================================================================
/**
    One TCP link to another peer (length-prefixed binary ValueTree messages).

    Everything is sent from a background thread with two queues: song edits
    and other small messages always go first, audio-file chunks (64 KB) fill
    the gaps. So a big upload never delays edits or blocks the window.
*/
class PeerConnection  : public juce::InterprocessConnection,
                        private juce::Thread
{
public:
    explicit PeerConnection (Session& s);
    ~PeerConnection() override;

    /** Queues a message (any thread). */
    void sendTree (const juce::ValueTree& msg);

    /** Queues an audio file to be sent in chunks (once per hash). */
    void queueAssetSend (const juce::String& hash, const juce::File& file);

    Session& session;
    int peerIndex = -1;          // assigned by the host
    juce::String peerName;
    int lastSeqApplied = 0;      // host side: last op batch received from this peer
    bool welcomed = false;
    juce::String viewing;        // host side: presence of this peer (pattern sid)
    bool recording = false;
    int colourIndex = -1;        // host side: this person's colour, kept for the whole connection
    std::atomic<bool> dead { false };
    const int tag;               // identifies this link for transfers

    static constexpr int assetChunkBytes = 64 * 1024;

private:
    struct AssetJob { juce::String hash; juce::File file; juce::int64 sent = 0, total = 0; };

    juce::CriticalSection queueLock;
    std::deque<juce::MemoryBlock> messages;
    std::deque<AssetJob> assetJobs;
    std::unique_ptr<juce::FileInputStream> currentAsset;
    juce::uint32 nextChunkTime = 0;   // for the upload speed limit

    void run() override;
    bool sendNextAssetChunk();

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
    void setPlaying (bool shouldPlay, double startSeconds = 0.0);

    /** Moves the playhead; while playing, everyone jumps there too. */
    void seek (double seconds);

    /** Pattern mode: which pattern plays. Shared too, so everyone hears the same thing. */
    void setPlayingPattern (const juce::String& patternSid);
    juce::String getPlayingPattern() const;

    /** Records every armed insert from this position (switches to SONG mode).
        Everyone else plays along from the same spot. False if nothing is armed. */
    bool startRecording (double startSeconds);
    bool isRecording() const                     { return recorder != nullptr && recorder->isRecording(); }
    Recorder& getRecorder() noexcept             { return *recorder; }

    //==============================================================================
    // People and chat
    struct Person
    {
        juce::String id, name, viewing;     // viewing = sid of the pattern they're looking at
        juce::Colour colour;
        bool isHost = false, isMe = false, recording = false;
    };

    struct ChatLine
    {
        juce::String name, text, time;      // time like "12:31"
        juce::Colour colour;
        bool system = false;                // "Ana joined" etc.
    };

    /** Your name as the others see it. */
    void setUserName (const juce::String&);
    juce::String getUserName() const              { return peerName; }
    /** Tells the others which pattern you're looking at. */
    void setViewingPattern (const juce::String& patternSid);
    /** Everyone in the session (just you when offline), host first. */
    juce::Array<Person> getPeople() const;
    void sendChat (const juce::String& text);
    const juce::Array<ChatLine>& getChat() const noexcept   { return chat; }
    static juce::Colour personColour (int index);

    std::function<void()> onPeopleChanged;
    std::function<void()> onChatChanged;

    /** PAT / SONG mode, shared like play/stop. */
    void setSongMode (bool shouldBeSong);
    bool isSongMode() const;
    PatternCompiler* getCompiler() const noexcept  { return compiler.get(); }

    /** This PC's shared audio files. Missing ones are fetched automatically. */
    AssetStore& getAssetStore() noexcept           { return assets; }
    void setAssetFolder (const juce::File& folder) { assets.setFolder (folder); }

    /** Caps how fast each link uploads audio files (bytes per second, 0 = no limit).
        The self-test uses it to imitate a home internet upload. */
    void setUploadLimit (int bytesPerSecond)       { uploadLimit = juce::jmax (0, bytesPerSecond); }
    int getUploadLimit() const noexcept            { return uploadLimit; }
    void logMessage (const juce::String& s)        { log (s); }
    MixerRouter* getRouter() const noexcept        { return router.get(); }

    std::function<void()> onEditAboutToBeReplaced;
    std::function<void()> onEditReplaced;
    std::function<void()> onRemoteChange;
    std::function<void()> onTransportChanged;
    std::function<void()> onStatusChanged;
    std::function<void (const juce::String&)> onLog;

    juce::String peerName = juce::SystemStats::getComputerName();

    static constexpr int protocolVersion = 6;
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
    AssetStore assets;
    std::unique_ptr<Recorder> recorder;
    juce::ValueTree deferredSnapshot;   // a fresh copy that arrived while recording
    std::atomic<int> uploadLimit { 0 };
    std::unique_ptr<te::Edit> edit;
    std::unique_ptr<EditSync> editSync;
    std::unique_ptr<PatternCompiler> compiler;
    std::unique_ptr<MixerRouter> router;
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
    int tick = 0, assetRequestRound = 0;
    std::map<juce::String, juce::uint32> lastAssetRequest;
    std::map<juce::String, juce::Array<int>> assetWantedBy;   // host: links waiting for a file it doesn't have yet

    // People and chat
    juce::ValueTree roster;              // PERSON children: built by the host, sent to everyone
    juce::String myRosterID { "0" };
    juce::String viewingPattern;
    bool presenceRecording = false;
    int nextColourIndex = 1;             // 0 is the host's
    juce::Array<ChatLine> chat;

    void sendPresence();
    void rebuildRoster();                // host
    void addChatLine (const ChatLine&);
    void broadcastSystemChat (const juce::String& text);   // host
    juce::Colour myColour() const;

    void installEdit (std::unique_ptr<te::Edit>);
    void attachSync();
    void sendLocalOp (const juce::ValueTree& op);
    void flushOutgoing();
    void broadcast (const juce::ValueTree& msg, PeerConnection* except);
    void applyOps (const juce::ValueTree& msg, PeerConnection* from);
    void applyTransport (const juce::ValueTree& msg);
    void sendTransport (bool patternOnly, const juce::String& recordingBy = {});
    void sendSnapshotTo (PeerConnection&, const juce::String& reason);
    void loadSnapshot (const juce::ValueTree& msg);
    void log (const juce::String&);
    void statusChanged();
    void checkAssets();
    void handleAssetAdded (const juce::String& hash);
    void handleAssetMessage (PeerConnection&, const juce::ValueTree&);
    PeerConnection* findLink (int tag) const;
    juce::String assetName (const juce::String& hash) const;
    void timerCallback() override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Session)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};

} // namespace collab

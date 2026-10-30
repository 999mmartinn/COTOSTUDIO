#pragma once

#include "EditSync.h"
#include "AssetStore.h"
#include "Recorder.h"
#include "ProjectFile.h"
#include "MixerRouter.h"
#include <deque>

class PatternCompiler;

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
    bool recording = false, focus = false, listening = false;
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
    /** Of those, how many were merged into the song in place (no reload of every plugin). */
    int getInPlaceResyncCount() const noexcept   { return inPlaceResyncs; }
    bool isInSync() const noexcept               { return lastHashMatched; }

    /** Playback (PAT/SONG, play/stop, position, which pattern plays) belongs to each PC:
        everyone listens to their own part. While you share your playback, everyone
        following your share hears exactly what you hear. While you follow someone
        else's share, these do nothing (only the person sharing controls playback). */
    void setPlaying (bool shouldPlay, double startSeconds = 0.0);

    /** Moves the playhead. */
    void seek (double seconds);

    /** Pattern mode: which pattern plays. */
    void setPlayingPattern (const juce::String& patternSid);
    juce::String getPlayingPattern() const;

    /** Records every armed insert from this position (switches to SONG mode).
        False if nothing is armed, or while you follow someone's share. */
    bool startRecording (double startSeconds);

    //==============================================================================
    // Solo: what you listen to. Personal like playback; people following your share
    // hear your solo too. Ignored while you follow someone's share.
    /** Solo on/off for a channel or a mixer insert (track ID). False if ignored. */
    bool toggleSolo (const juce::String& trackID, bool isInsert);
    bool isSoloed (const juce::String& trackID) const;
    /** True if this track can't be heard on this PC right now because of a solo. */
    bool isSilencedBySolo (const juce::String& trackID) const;
    void clearSolo();

    // Metronome (personal) and count-in before recording
    void setMetronome (bool);
    bool isMetronomeOn() const noexcept            { return metronome; }
    void setCountInBars (int bars);
    int getCountInBars() const;

    //==============================================================================
    // Sharing playback ("listen together") and Focus
    /** Starts sharing your playback: everyone who isn't in Focus (or recording) follows it.
        Only one person shares at a time. Returns an error text, or "" if it worked. */
    juce::String startSharing();
    /** Ends your share: everyone goes back to their own playback, as it was before. */
    void stopSharing();
    /** Stops following someone's share (only for you; you're back to where you were). */
    void leaveShare();
    /** Follows the running share again (after leaving it, or while in Focus). */
    void joinShare();
    /** Focus: a share started by someone else doesn't pull you in. Turning it on while
        following a share leaves it; turning it off joins a running share. */
    void setFocus (bool);
    bool isFocus() const noexcept                  { return focus; }

    bool isShareRunning() const noexcept           { return shareOwner.isNotEmpty(); }
    bool isSharing() const noexcept                { return isShareRunning() && shareOwner == myRosterID; }
    bool isFollowingShare() const noexcept         { return following; }
    bool canControlPlayback() const noexcept       { return ! following; }
    /** Who is sharing ("" if nobody), and their colour. */
    juce::String getShareOwnerName() const;
    juce::Colour getShareOwnerColour() const;

    /** While following a share in PAT mode: the person sharing plays a pattern that isn't
        the one you're looking at. The window asks whether to go there. */
    std::function<void (const juce::String& patternSid, const juce::String& ownerName)> onSharedPatternOffered;

    //==============================================================================
    // Tempo: part of the song, so in a session everyone else has to agree to a change.
    struct TempoQuestion
    {
        int id = 0;
        double bpm = 120.0, oldBpm = 120.0;
        juce::String fromName;
    };

    /** True when a tempo change needs the others' approval (someone else is connected). */
    bool tempoNeedsApproval() const;
    /** Changes the tempo (alone: right away; in a session: asks everyone else first).
        Tempo changes are not part of Ctrl+Z (undoing would skip the approval). */
    void requestTempo (double bpm);
    /** Your own request still waiting for answers (0 if none). */
    double getPendingTempo() const noexcept        { return pendingTempo; }
    /** Someone asks to change the tempo: answer with answerTempo (id, yes). */
    std::function<void (const TempoQuestion&)> onTempoQuestion;
    /** The question is over (decided, cancelled or expired): close it if it's still shown. */
    std::function<void (int id)> onTempoQuestionClosed;
    void answerTempo (int id, bool yes);
    /** How long the others have to answer (the self-test shortens it). */
    void setTempoAnswerTimeout (int ms)            { tempoAnswerTimeoutMs = juce::jmax (500, ms); }
    bool isRecording() const                     { return recorder != nullptr && recorder->isRecording(); }
    Recorder& getRecorder() noexcept             { return *recorder; }

    //==============================================================================
    // Song files (see ProjectFile.h)
    /** Saves this PC's copy of the song (in the background). includeAudio: put every
        audio file in it too, so it opens anywhere. asBackup: an automatic backup (with its
        audio, sharing it with the other backups in that folder, which is then tidied up:
        see Backups.h). */
    void saveProject (const juce::File&, bool includeAudio, std::function<void (project::SaveResult)> onDone,
                      bool asBackup = false);
    /** Opens a song file (in the background). Only when you're alone or hosting:
        it replaces the song for everyone. onDone gets an error text, or "" if it worked. */
    void openProject (const juce::File&, std::function<void (juce::String error)> onDone,
                      const juce::String& titleOverride = {});
    /** Starts a new song (the default one). Same rule as openProject. Returns an error or "". */
    juce::String newProject();
    /** Starts a new, empty song (no channels, no patterns) that `fill` builds before it
        replaces the current one (e.g. an imported FL Studio template). */
    juce::String newProject (std::function<void (te::Edit&)> fill, const juce::String& title = "Untitled");
    bool canReplaceSong() const noexcept           { return mode == Mode::offline || mode == Mode::hosting; }
    bool isBusyWithFile() const noexcept           { return fileJobsRunning > 0; }
    /** Why the song was last replaced: "own" (new/open here), "join", "newSong" (the host
        switched songs) or a resync reason. */
    juce::String getLastReplaceReason() const      { return lastReplaceReason; }
    juce::String getSongTitle() const;
    /** This PC's Tailscale address (100.64.x.x - 100.127.x.x), or "" if Tailscale isn't running. */
    static juce::String findTailscaleAddress();

    //==============================================================================
    // People and chat
    struct Person
    {
        juce::String id, name, viewing;     // viewing = sid of the pattern they're looking at
        juce::Colour colour;
        bool isHost = false, isMe = false, recording = false;
        bool focus = false, listening = false, sharing = false;   // sharing playback / following it / in Focus
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

    /** PAT / SONG mode (yours, like play/stop). */
    void setSongMode (bool shouldBeSong);
    bool isSongMode() const;
    PatternCompiler* getCompiler() const noexcept  { return compiler.get(); }

    /** This PC's shared audio files. Missing ones are fetched automatically. */
    AssetStore& getAssetStore() noexcept           { return assets; }
    void setAssetFolder (const juce::File& folder) { assets.setFolder (folder); }

    /** The file the song was last saved to or opened from (none = never saved). The audio
        it keeps in its project folder is never cleaned up. Set by the main window. */
    void setSongFile (const juce::File& f)         { songFile = f; }
    /** The song is about to be closed (new / open / template / join / quit): the audio files
        that came into this PC's cache while it was open and that its saved project doesn't
        keep (recorded, imported, received - never saved) are deleted once it is gone,
        unless the next song uses them. */
    void forgetUnsavedAudio();
    /** What forgetUnsavedAudio() would delete right now (for tests and the log). */
    juce::StringArray getUnsavedAudio() const;

    /** Caps how fast each link uploads audio files (bytes per second, 0 = no limit).
        The self-test uses it to imitate a home internet upload. */
    void setUploadLimit (int bytesPerSecond)       { uploadLimit = juce::jmax (0, bytesPerSecond); }
    int getUploadLimit() const noexcept            { return uploadLimit; }
    void logMessage (const juce::String& s)        { log (s); }
    MixerRouter* getRouter() const noexcept        { return router.get(); }

    /** Disconnects every callback (the window calls it before it goes away). */
    void clearCallbacks()
    {
        onEditAboutToBeReplaced = onEditReplaced = onRemoteChange = onTransportChanged = onStatusChanged = nullptr;
        onPeopleChanged = onChatChanged = nullptr;
        onLog = nullptr;
        onSharedPatternOffered = nullptr;
        onTempoQuestion = nullptr;
        onTempoQuestionClosed = nullptr;
    }

    std::function<void()> onEditAboutToBeReplaced;
    std::function<void()> onEditReplaced;
    std::function<void()> onRemoteChange;
    std::function<void()> onTransportChanged;
    std::function<void()> onStatusChanged;
    std::function<void (const juce::String&)> onLog;

    juce::String peerName = juce::SystemStats::getComputerName();

    static constexpr int protocolVersion = 10;  // 9 (1.2): 200 playlist tracks instead of 16
                                                // 10 (1.12.2): changes carry the song's number; fresh copies merge in place
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
    juce::File songFile;
    juce::StringArray audioToForget;   // set when the song is being closed
    bool songClosing = false;
    void forgetAudioNow (te::Edit* nextSong);
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
    int songNumber = 0;          // which song changes belong to (the host counts up on every new song)
    int inPlaceResyncs = 0;      // fresh copies merged without reloading the song
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

    void sendPresence (const juce::String& shareNote = {});
    void rebuildRoster();                // host

    // Playback of this PC (kept while the song object is replaced, restored after a share)
    struct LocalPlayback
    {
        bool songMode = false, playing = false;
        juce::String pattern;
        double position = 0.0;
    };

    LocalPlayback captureLocalPlayback() const;
    void restoreLocalPlayback (const LocalPlayback&, bool resumePlaying);

    // Solo and metronome (this PC's)
    MixerRouter::Solo mySolo, sharedSolo;   // sharedSolo: the solo of the share you follow
    bool metronome = false;
    void applySolo();

    // Sharing (see startSharing). The host decides who shares; everyone learns it from the roster.
    juce::String shareOwner;             // roster id of the person sharing ("" = nobody)
    bool following = false, focus = false;
    LocalPlayback beforeShare;           // what this PC was doing before it followed the share
    juce::StringArray shareListenersSeen;   // owner: who already got the current playback
    juce::String lastOfferedPattern;
    void setShareOwner (const juce::String& newOwner);      // host: decides; everyone: reacts
    void resetSessionOnlyState();        // leaving a session: own playback back, no questions open
    void shareOwnerChanged (const juce::String& oldOwner);
    void followShare (bool manual);
    void unfollowShare (const juce::String& note);
    void sendShareToNewListeners();
    void handleShareRequest (const juce::String& fromID, const juce::String& fromName, bool start);   // host
    void handleShareNote (const juce::String& name, const juce::String& note);                         // host
    juce::String nameOf (const juce::String& rosterID) const;

    // Tempo approval (the host runs the vote)
    struct TempoVote
    {
        int id = 0;
        double bpm = 120.0, oldBpm = 120.0;
        juce::String fromID, fromName;
        juce::StringArray waiting;      // roster ids that haven't answered yet
        juce::uint32 deadline = 0;
    };

    TempoVote tempoVote;                 // host: the running vote (id 0 = none)
    int nextTempoID = 1;
    int tempoAnswerTimeoutMs = 30000;
    double pendingTempo = 0.0;           // my own request, waiting
    juce::Array<int> openTempoQuestions; // questions shown on this PC
    void applyTempo (double bpm);
    void startTempoVote (const juce::String& fromID, const juce::String& fromName, double bpm);   // host
    void handleTempoAnswer (int id, const juce::String& fromID, bool yes);                         // host
    void finishTempoVote (const juce::String& result, const juce::String& chatText);               // host
    void showTempoQuestion (const juce::ValueTree& msg);
    void closeTempoQuestion (int id, const juce::String& requesterID, double bpm);
    void checkTempoVote();               // host: answers / timeout / people who left
    void addChatLine (const ChatLine&);
    void broadcastSystemChat (const juce::String& text);   // host
    juce::Colour myColour() const;

    void installEdit (std::unique_ptr<te::Edit>, bool assignSyncIDs = false);
    void attachSync();
    void sendLocalOp (const juce::ValueTree& op);
    void flushOutgoing();
    void broadcast (const juce::ValueTree& msg, PeerConnection* except);
    /** Applies the changes in an "ops" message; returns the ones that applied (to relay). */
    juce::ValueTree applyOps (const juce::ValueTree& msg, PeerConnection* from);
    void applyTransport (const juce::ValueTree& msg);
    /** Sends this PC's playback to the people following its share (only while sharing).
        to: one person's roster id, or "" for everyone. */
    void sendTransport (bool patternOnly, const juce::String& recordingBy = {}, const juce::String& to = {});
    void sendSnapshotTo (PeerConnection&, const juce::String& reason, const juce::ValueTree& premade = {});
    juce::String replaceSong (std::unique_ptr<te::Edit>, const juce::String& announcement);
    juce::ThreadPool fileJobs { 1 };
    std::atomic<int> fileJobsRunning { 0 };
    juce::String lastReplaceReason { "own" };
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

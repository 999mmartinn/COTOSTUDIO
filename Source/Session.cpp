#include "Session.h"
#include "Backups.h"
#include "SongModel.h"
#include "PatternCompiler.h"
#include "AppData.h"
#include "MixerRouter.h"
#include "ProjectFile.h"

namespace collab
{

//==============================================================================
static juce::MemoryBlock toBlock (const juce::ValueTree& v)
{
    juce::MemoryOutputStream out;
    v.writeToStream (out);
    return out.getMemoryBlock();
}

static std::atomic<int> nextConnectionTag { 1 };

PeerConnection::PeerConnection (Session& s)
    : InterprocessConnection (true, Session::magicNumber),
      juce::Thread ("SyncTest sender"),
      session (s), tag (nextConnectionTag++)
{
    startThread();
}

PeerConnection::~PeerConnection()
{
    dead = true;
    signalThreadShouldExit();
    notify();
    stopThread (4000);
    disconnect();
}

void PeerConnection::sendTree (const juce::ValueTree& msg)
{
    if (dead)
        return;

    {
        const juce::ScopedLock sl (queueLock);
        messages.push_back (toBlock (msg));
    }

    notify();
}

void PeerConnection::queueAssetSend (const juce::String& hash, const juce::File& file)
{
    if (dead || ! file.existsAsFile())
        return;

    {
        const juce::ScopedLock sl (queueLock);

        for (auto& j : assetJobs)
            if (j.hash == hash)
                return;     // already queued

        assetJobs.push_back ({ hash, file, 0, file.getSize() });
    }

    notify();
}

void PeerConnection::run()
{
    while (! threadShouldExit())
    {
        bool didSomething = false;

        // 1. All waiting messages first (song edits, transport, hashes...).
        for (;;)
        {
            juce::MemoryBlock next;

            {
                const juce::ScopedLock sl (queueLock);

                if (messages.empty())
                    break;

                next = std::move (messages.front());
                messages.pop_front();
            }

            if (! dead && isConnected())
                sendMessage (next);

            didSomething = true;
        }

        // 2. Then one chunk of a file, and check the message queue again.
        //    With an upload limit, chunks are spaced out; messages never wait for that.
        const int limit = session.getUploadLimit();
        const auto now = juce::Time::getMillisecondCounter();
        const bool chunkAllowed = limit <= 0 || (int) (nextChunkTime - now) <= 0;

        if (chunkAllowed && ! dead && isConnected() && sendNextAssetChunk())
        {
            didSomething = true;

            if (limit > 0)
                nextChunkTime = juce::Time::getMillisecondCounter() + (juce::uint32) ((juce::int64) assetChunkBytes * 1000 / limit);
        }

        if (! didSomething)
            wait (chunkAllowed ? 50 : juce::jlimit (1, 50, (int) (nextChunkTime - now)));
    }
}

bool PeerConnection::sendNextAssetChunk()
{
    AssetJob job;

    {
        const juce::ScopedLock sl (queueLock);

        if (assetJobs.empty())
            return false;

        job = assetJobs.front();
    }

    if (currentAsset == nullptr)
    {
        currentAsset = std::make_unique<juce::FileInputStream> (job.file);

        if (currentAsset->failedToOpen())
        {
            currentAsset.reset();
            const juce::ScopedLock sl (queueLock);
            assetJobs.pop_front();
            return true;
        }
    }

    juce::MemoryBlock data ((size_t) juce::jmin ((juce::int64) assetChunkBytes, job.total - job.sent));
    const auto offset = job.sent;
    const auto read = currentAsset->read (data.getData(), (int) data.getSize());

    if (read <= 0)
    {
        currentAsset.reset();
        const juce::ScopedLock sl (queueLock);
        assetJobs.pop_front();
        return true;
    }

    data.setSize ((size_t) read);

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "asset", nullptr);
    msg.setProperty (ids::hash, job.hash, nullptr);
    msg.setProperty ("offset", offset, nullptr);
    msg.setProperty ("total", job.total, nullptr);
    msg.setProperty ("data", data, nullptr);
    sendMessage (toBlock (msg));

    {
        const juce::ScopedLock sl (queueLock);

        if (! assetJobs.empty())
        {
            assetJobs.front().sent += read;

            if (assetJobs.front().sent >= assetJobs.front().total)
            {
                assetJobs.pop_front();
                currentAsset.reset();
            }
        }
    }

    return true;
}

void PeerConnection::connectionMade()    { session.handleConnectionMade (*this); }
void PeerConnection::connectionLost()    { dead = true; session.handleConnectionLost (*this); }

void PeerConnection::messageReceived (const juce::MemoryBlock& data)
{
    auto msg = juce::ValueTree::readFromData (data.getData(), data.getSize());

    if (msg.isValid())
        session.handleMessage (*this, msg);
}

//==============================================================================
class Session::Server  : public juce::InterprocessConnectionServer
{
public:
    explicit Server (Session& s) : session (s) {}
    ~Server() override { stop(); }

    // Runs on the server thread; ownership goes to the session under its lock.
    juce::InterprocessConnection* createConnectionObject() override
    {
        auto c = new PeerConnection (session);
        session.clients.add (c);
        return c;
    }

    Session& session;
};

//==============================================================================
static juce::File defaultAssetFolder()
{
    return appdata::audioCacheFolder (1);
}

Session::Session (te::Engine& e) : engine (e), assets (defaultAssetFolder())
{
    assets.onAssetAdded = [this] (const juce::String& hash) { handleAssetAdded (hash); };

    recorder = std::make_unique<Recorder> (engine, assets);
    recorder->onLog = [this] (const juce::String& s) { log (s); };
    recorder->onTakeAdded = [this] (const juce::ValueTree&)
    {
        if (onRemoteChange)
            onRemoteChange();   // refresh the screens
    };

    startTimer (500);
}

Session::~Session()
{
    fileJobs.removeAllJobs (true, 60000);
    stopTimer();
    leave();
    recorder.reset();   // detaches from the Edit while it still exists
    router.reset();
    compiler.reset();
    editSync.reset();
    edit.reset();

    // Closing the app without saving: audio that only this unsaved song used goes.
    if (songClosing)
        forgetAudioNow (nullptr);
}

//==============================================================================
juce::StringArray Session::getUnsavedAudio() const
{
    // Kept: what the saved project has in its folder. An old-format song (audio inside the
    // file) or a song file that can't be read keeps everything, to be safe.
    std::set<juce::String> kept;

    if (songFile.existsAsFile())
    {
        if (! songFile.hasFileExtension (appdata::songExtension))
            return {};

        for (auto& [path, hash] : project::readAudioList (songFile))
            kept.insert (hash);
    }

    juce::StringArray result;

    for (auto& h : assets.getAddedSinceMark())
        if (kept.count (h) == 0)
            result.add (h);

    return result;
}

void Session::forgetUnsavedAudio()
{
    audioToForget = getUnsavedAudio();
    songClosing = true;
}

void Session::forgetAudioNow (te::Edit* nextSong)
{
    songClosing = false;
    auto list = audioToForget;
    audioToForget.clear();

    // Never what the next song uses (e.g. a file both songs have).
    if (nextSong != nullptr)
        for (auto& h : song::getAssetHashes (*nextSong))
            list.removeString (h);

    if (! list.isEmpty())
    {
        engine.getAudioFileManager().releaseAllFiles();   // (Windows can't delete open files)
        assets.deleteFiles (list);
        log ("Removed " + juce::String (list.size()) + " unsaved audio file(s) of the closed song from this PC's cache");
    }

    assets.markNewSong();
}

void Session::log (const juce::String& s)
{
    DBG (s);

    if (onLog)
        onLog (s);
}

void Session::statusChanged()
{
    if (onStatusChanged)
        onStatusChanged();
}

int Session::getNumConnectedPeers() const
{
    if (mode == Mode::joined)
        return 1;

    int n = 0;
    const juce::ScopedLock sl (clients.getLock());

    for (auto* c : clients)
        if (c->welcomed && ! c->dead)
            ++n;

    return n;
}

juce::String Session::getStatusText() const
{
    switch (mode)
    {
        case Mode::offline:     return TRANS ("Offline");
        case Mode::hosting:
        {
            const auto tailscale = findTailscaleAddress();
            return TRANS ("Hosting - {N} peer(s) connected").replace ("{N}", juce::String (getNumConnectedPeers()))
                     + (tailscale.isNotEmpty() ? " - " + TRANS ("friends join with") + " " + tailscale : juce::String());
        }
        case Mode::connecting:  return TRANS ("Connecting...");
        case Mode::joined:      return TRANS ("Joined as peer") + " " + juce::String (peerIndex)
                                       + " - " + (lastHashMatched ? TRANS ("in sync") : TRANS ("checking sync..."));
    }

    return {};
}

//==============================================================================
static void setUpLocalTransport (te::Edit& edit)
{
    // Pattern mode: the PatternCompiler sets the loop to the playing pattern.
    edit.getTransport().looping = true;
    edit.playInStopEnabled = true;
    edit.getTransport().ensureContextAllocated();
}

void Session::createLocalEdit()
{
    lastReplaceReason = "own";
    auto newEdit = te::createEmptyEdit (engine, juce::File());
    song::createDefaultSong (*newEdit);
    installEdit (std::move (newEdit), true);
}

void Session::installEdit (std::unique_ptr<te::Edit> newEdit, bool assignSyncIDs)
{
    if (onEditAboutToBeReplaced)
        onEditAboutToBeReplaced();

    recorder->attachTo (nullptr);
    router.reset();
    compiler.reset();
    editSync.reset();
    edit.reset();
    edit = std::move (newEdit);

    // Fix this Edit's ID counter now, inside our own range, before any remote
    // items arrive (Tracktion picks "highest existing ID + 1" the first time).
    edit->createNewItemID();

    setUpLocalTransport (*edit);
    attachSync();
    compiler = std::make_unique<PatternCompiler> (*edit, assets);
    router = std::make_unique<MixerRouter> (*edit);
    recorder->attachTo (edit.get());
    applySolo();
    edit->clickTrackEnabled = metronome;   // personal: not part of the shared song

    // Our own new songs get their sync IDs now, before the window takes the song's
    // fingerprint (otherwise an untouched new song would look changed).
    if (assignSyncIDs)
        editSync->assignMissingSyncIDs();

    // The old song was closed on purpose (not a resync): its unsaved audio goes now.
    if (songClosing)
        forgetAudioNow (edit.get());

    if (onEditReplaced)
        onEditReplaced();
}

void Session::attachSync()
{
    editSync = std::make_unique<EditSync> (*edit);
    editSync->onLocalOp = [this] (const juce::ValueTree& op) { sendLocalOp (op); };
    editSync->onLog = [this] (const juce::String& s) { log (s); };
}

//==============================================================================
// People and chat
static const juce::Identifier songNumberID ("song");
static const juce::Identifier ROSTER ("ROSTER"), PERSON ("PERSON"), LINE ("LINE"),
                              viewingID ("viewing"), recordingID ("recording"), colourID ("colour"),
                              hostID ("host"), yourID ("yourId"), textID ("text"), systemID ("system"), timeID ("time"),
                              focusID ("focus"), listeningID ("listening"), shareID ("share"), shareNoteID ("shareNote"),
                              sharerID ("sharer"), toID ("to"), bpmID ("bpm"), oldBpmID ("oldBpm"), resultID ("result"),
                              yesID ("yes"), fromID_ ("from"), soloID ("solo");

juce::Colour Session::personColour (int index)
{
    static const juce::uint32 palette[] = { 0xfff0a030, 0xff4fb0f0, 0xffe05a8a, 0xff6fd06f,
                                            0xffb48cf0, 0xfff0e060, 0xff50d0c0, 0xfff07050 };
    return juce::Colour (palette[(size_t) juce::jmax (0, index) % std::size (palette)]);
}

static juce::ValueTree chatToTree (const Session::ChatLine& line)
{
    juce::ValueTree v (LINE);
    v.setProperty (ids::name, line.name, nullptr);
    v.setProperty (textID, line.text, nullptr);
    v.setProperty (timeID, line.time, nullptr);
    v.setProperty (colourID, line.colour.toString(), nullptr);
    v.setProperty (systemID, line.system, nullptr);
    return v;
}

static Session::ChatLine treeToChat (const juce::ValueTree& v)
{
    Session::ChatLine line;
    line.name = v[ids::name].toString().substring (0, 40);
    line.text = v[textID].toString().substring (0, 500);
    line.time = v[timeID].toString().substring (0, 8);
    line.colour = juce::Colour::fromString (v[colourID].toString());
    line.system = v[systemID];
    return line;
}

static juce::String timeNow()
{
    return juce::Time::getCurrentTime().formatted ("%H:%M");
}

juce::Array<Session::Person> Session::getPeople() const
{
    juce::Array<Person> list;

    if ((mode != Mode::hosting && mode != Mode::joined) || roster.getNumChildren() == 0)
    {
        Person me;
        me.id = "0";
        me.name = peerName;
        me.viewing = viewingPattern;
        me.colour = personColour (0);
        me.isMe = true;
        me.recording = isRecording();
        me.focus = focus;
        list.add (me);
        return list;
    }

    for (auto p : roster)
    {
        Person x;
        x.id = p[ids::id].toString();
        x.name = p[ids::name].toString();
        x.viewing = p[viewingID].toString();
        x.colour = personColour ((int) p[colourID]);
        x.isHost = p[hostID];
        x.recording = p[recordingID];
        x.focus = p[focusID];
        x.listening = p[listeningID];
        x.sharing = x.id == shareOwner;
        x.isMe = x.id == myRosterID;
        list.add (x);
    }

    return list;
}

juce::Colour Session::myColour() const
{
    for (auto& p : getPeople())
        if (p.isMe)
            return p.colour;

    return personColour (0);
}

void Session::setUserName (const juce::String& newName)
{
    const auto name = newName.trim().substring (0, 30);

    if (name.isEmpty() || name == peerName)
        return;

    peerName = name;
    sendPresence();
}

void Session::setViewingPattern (const juce::String& patternSid)
{
    if (patternSid == viewingPattern)
        return;

    viewingPattern = patternSid;
    sendPresence();
}

void Session::sendPresence (const juce::String& shareNote)
{
    if (mode == Mode::hosting)
    {
        rebuildRoster();

        if (shareNote.isNotEmpty())
            handleShareNote (peerName, shareNote);
    }
    else if (mode == Mode::joined && hostLink != nullptr)
    {
        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "presence", nullptr);
        msg.setProperty (ids::name, peerName, nullptr);
        msg.setProperty (viewingID, viewingPattern, nullptr);
        msg.setProperty (recordingID, isRecording(), nullptr);
        msg.setProperty (focusID, focus, nullptr);
        msg.setProperty (listeningID, following, nullptr);

        if (shareNote.isNotEmpty())
            msg.setProperty (shareNoteID, shareNote, nullptr);

        hostLink->sendTree (msg);
    }
    else if (onPeopleChanged)
    {
        onPeopleChanged();
    }
}

void Session::rebuildRoster()
{
    if (mode != Mode::hosting)
        return;

    juce::ValueTree r (ROSTER);

    auto addPerson = [&r] (const juce::String& id, const juce::String& name, const juce::String& viewing,
                           bool recording, int colour, bool host, bool inFocus, bool listening)
    {
        juce::ValueTree p (PERSON);
        p.setProperty (ids::id, id, nullptr);
        p.setProperty (ids::name, name, nullptr);
        p.setProperty (viewingID, viewing, nullptr);
        p.setProperty (recordingID, recording, nullptr);
        p.setProperty (colourID, colour, nullptr);
        p.setProperty (hostID, host, nullptr);
        p.setProperty (focusID, inFocus, nullptr);
        p.setProperty (listeningID, listening, nullptr);
        r.appendChild (p, nullptr);
    };

    r.setProperty (shareID, shareOwner, nullptr);
    addPerson ("0", peerName, viewingPattern, isRecording(), 0, true, focus, following);

    {
        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
            if (c->welcomed && ! c->dead)
                addPerson (juce::String (c->tag), c->peerName, c->viewing, c->recording, c->colourIndex, false,
                           c->focus, c->listening && juce::String (c->tag) != shareOwner);
    }

    if (roster.isEquivalentTo (r))
        return;

    roster = r;
    myRosterID = "0";
    sendShareToNewListeners();   // when the host shares: newcomers to the share get the playback

    {
        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
        {
            if (! c->welcomed || c->dead)
                continue;

            juce::ValueTree msg (ids::MSG);
            msg.setProperty (ids::type, "roster", nullptr);
            msg.setProperty (yourID, juce::String (c->tag), nullptr);
            msg.appendChild (r.createCopy(), nullptr);
            c->sendTree (msg);
        }
    }

    if (onPeopleChanged)
        onPeopleChanged();
}

void Session::addChatLine (const ChatLine& line)
{
    chat.add (line);

    while (chat.size() > 300)
        chat.remove (0);

    if (onChatChanged)
        onChatChanged();
}

void Session::broadcastSystemChat (const juce::String& text)
{
    ChatLine line;
    line.text = text;
    line.time = timeNow();
    line.colour = juce::Colours::grey;
    line.system = true;
    addChatLine (line);

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "chat", nullptr);
    msg.appendChild (chatToTree (line), nullptr);
    broadcast (msg, nullptr);
}

void Session::sendChat (const juce::String& textIn)
{
    const auto text = textIn.trim().substring (0, 500);

    if (text.isEmpty())
        return;

    ChatLine line;
    line.name = peerName;
    line.text = text;
    line.time = timeNow();
    line.colour = myColour();
    addChatLine (line);

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "chat", nullptr);
    msg.appendChild (chatToTree (line), nullptr);

    if (mode == Mode::hosting)
        broadcast (msg, nullptr);
    else if (mode == Mode::joined && hostLink != nullptr)
        hostLink->sendTree (msg);
}

//==============================================================================
bool Session::startHosting (int port)
{
    leave();

    if (edit == nullptr)
        createLocalEdit();

    server = std::make_unique<Server> (*this);

    if (! server->beginWaitingForSocket (port))
    {
        server.reset();
        log ("Could not listen on port " + juce::String (port) + " (is it already in use?)");
        return false;
    }

    mode = Mode::hosting;
    peerIndex = 0;
    log ("Hosting on port " + juce::String (port));

    const auto tailscale = findTailscaleAddress();
    log (tailscale.isNotEmpty() ? "Your friends join with your Tailscale address: " + tailscale
                                : juce::String ("Tailscale isn't running on this PC; friends on your Wi-Fi can join with your local address"));
    rebuildRoster();
    statusChanged();
    return true;
}

bool Session::join (const juce::String& hostAddress, int port)
{
    leave();

    hostLink = std::make_unique<PeerConnection> (*this);
    mode = Mode::connecting;
    statusChanged();

    if (! hostLink->connectToSocket (hostAddress, port, 4000))
    {
        hostLink.reset();
        mode = Mode::offline;
        log ("Could not connect to " + hostAddress + ":" + juce::String (port));
        statusChanged();
        return false;
    }

    log ("Connected to " + hostAddress + ":" + juce::String (port) + ", waiting for the song...");
    return true;
}

void Session::leave()
{
    if (server != nullptr)
        server->stop();

    server.reset();

    {
        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
            assets.cancelDownloadsFrom (c->tag);
    }

    if (hostLink != nullptr)
        assets.cancelDownloadsFrom (hostLink->tag);

    assetWantedBy.clear();

    {
        const juce::ScopedLock sl (clients.getLock());
        clients.clear();
    }

    hostLink.reset();
    pendingOps.removeAllChildren (nullptr);
    resetSessionOnlyState();
    roster = {};
    myRosterID = "0";
    nextColourIndex = 1;

    if (mode != Mode::offline)
    {
        mode = Mode::offline;
        log ("Left the session");
        statusChanged();

        if (onPeopleChanged)
            onPeopleChanged();
    }
}

//==============================================================================
void Session::handleConnectionMade (PeerConnection& c)
{
    if (&c == hostLink.get())
    {
        juce::ValueTree hello (ids::MSG);
        hello.setProperty (ids::type, "hello", nullptr);
        hello.setProperty (ids::protocol, protocolVersion, nullptr);
        hello.setProperty (ids::peerName, peerName, nullptr);
        c.sendTree (hello);
    }
}

void Session::handleConnectionLost (PeerConnection& c)
{
    assets.cancelDownloadsFrom (c.tag);

    if (&c == hostLink.get())
    {
        log ("Lost the connection to the host. You keep your copy of the song.");
        resetSessionOnlyState();
        mode = Mode::offline;
        juce::MessageManager::callAsync ([weak = juce::WeakReference<Session> (this)]
                                        { if (weak != nullptr) weak->hostLink.reset(); });
    }
    else
    {
        log ((c.peerName.isNotEmpty() ? c.peerName : juce::String ("A peer")) + " left");

        if (c.welcomed && mode == Mode::hosting)
        {
            broadcastSystemChat ((c.peerName.isNotEmpty() ? c.peerName : juce::String ("A peer")) + " left");

            if (shareOwner == juce::String (c.tag))
            {
                broadcastSystemChat (c.peerName + "'s shared playback ended");
                setShareOwner ({});
            }

            rebuildRoster();   // c is marked dead, so it's no longer listed
            checkTempoVote();  // they don't hold up a tempo question any more
        }
    }

    if (onPeopleChanged)
        onPeopleChanged();

    statusChanged();
}

void Session::handleMessage (PeerConnection& from, const juce::ValueTree& msg)
{
    const auto type = msg[ids::type].toString();

    if (type == "assetGet" || type == "asset")
    {
        handleAssetMessage (from, msg);
        return;
    }

    if (mode == Mode::hosting)
    {
        if (type == "hello")
        {
            if ((int) msg[ids::protocol] != protocolVersion)
            {
                log ("Rejected " + msg[ids::peerName].toString() + ": different COTOSTUDIO version (everyone needs the same one)");
                from.disconnect();
                return;
            }

            from.peerName = msg[ids::peerName].toString().substring (0, 30);
            sendSnapshotTo (from, "join");
            log (from.peerName + " joined as peer " + juce::String (from.peerIndex));

            if (from.colourIndex < 0)
                from.colourIndex = nextColourIndex++;

            // The newcomer sees the recent chat, then everyone sees that they joined.
            juce::ValueTree history (ids::MSG);
            history.setProperty (ids::type, "chatHistory", nullptr);

            for (int i = juce::jmax (0, chat.size() - 50); i < chat.size(); ++i)
                history.appendChild (chatToTree (chat.getReference (i)), nullptr);

            from.sendTree (history);
            broadcastSystemChat (from.peerName + " joined");
            rebuildRoster();
            statusChanged();
        }
        else if (type == "ops")
        {
            from.lastSeqApplied = (int) msg[ids::seq];

            // Changes made to the song before this one (sent while the new one was on its
            // way): nothing to apply them to. Dropping them is right, they died with it.
            if ((int) msg[songNumberID] != songNumber)
                return;

            auto applied = applyOps (msg, &from);

            // Relay only what applied here: a change the host couldn't apply would fail on
            // everyone else too, and make each of them fetch a fresh copy.
            if (applied.getNumChildren() > 0)
                broadcast (applied, &from);     // relay to everyone else
        }
        else if (type == "transport")
        {
            // Only the person sharing controls playback.
            if (juce::String (from.tag) != shareOwner)
                return;

            auto relay = msg.createCopy();
            relay.setProperty (sharerID, juce::String (from.tag), nullptr);
            const auto to = msg[toID].toString();

            if (to.isEmpty())
            {
                applyTransport (relay);
                broadcast (relay, &from);
            }
            else if (to == "0")
            {
                applyTransport (relay);
            }
            else
            {
                flushOutgoing();
                const juce::ScopedLock sl (clients.getLock());

                for (auto* c : clients)
                    if (juce::String (c->tag) == to && c->welcomed && ! c->dead)
                        c->sendTree (relay);
            }
        }
        else if (type == "resync")
        {
            sendSnapshotTo (from, "resync");
        }
        else if (type == "presence")
        {
            const auto name = msg[ids::name].toString().trim().substring (0, 30);

            if (name.isNotEmpty())
                from.peerName = name;

            const bool startedRecording = ! from.recording && (bool) msg[recordingID];
            from.viewing = msg[viewingID].toString();
            from.recording = msg[recordingID];
            from.focus = msg[focusID];
            from.listening = msg[listeningID];
            rebuildRoster();

            if (startedRecording)
                broadcastSystemChat (from.peerName + " started recording");

            if (msg.hasProperty (shareNoteID))
                handleShareNote (from.peerName, msg[shareNoteID].toString());
        }
        else if (type == "shareStart" || type == "shareStop")
        {
            handleShareRequest (juce::String (from.tag), from.peerName, type == "shareStart");
        }
        else if (type == "tempoAsk")
        {
            startTempoVote (juce::String (from.tag), from.peerName, (double) msg[bpmID]);
        }
        else if (type == "tempoAnswer")
        {
            handleTempoAnswer ((int) msg[ids::id], juce::String (from.tag), (bool) msg[yesID]);
        }
        else if (type == "chat" && msg.getNumChildren() == 1)
        {
            // Name and colour come from the connection, not from the message.
            auto line = treeToChat (msg.getChild (0));
            line.name = from.peerName;
            line.colour = personColour (from.colourIndex);
            line.system = false;

            if (line.text.trim().isNotEmpty())
            {
                addChatLine (line);
                juce::ValueTree relay (ids::MSG);
                relay.setProperty (ids::type, "chat", nullptr);
                relay.appendChild (chatToTree (line), nullptr);
                broadcast (relay, &from);
            }
        }

        return;
    }

    // Joining side
    if (type == "roster" && msg.getNumChildren() == 1)
    {
        roster = msg.getChild (0).createCopy();
        myRosterID = msg[yourID].toString();

        const auto newOwner = roster[shareID].toString();

        if (newOwner != shareOwner)
        {
            const auto old = shareOwner;
            shareOwner = newOwner;
            shareOwnerChanged (old);
        }

        sendShareToNewListeners();

        if (onPeopleChanged)
            onPeopleChanged();

        return;
    }

    if (type == "chat" && msg.getNumChildren() == 1)
    {
        addChatLine (treeToChat (msg.getChild (0)));
        return;
    }

    if (type == "chatHistory")
    {
        chat.clear();

        for (auto line : msg)
            chat.add (treeToChat (line));

        if (onChatChanged)
            onChatChanged();

        return;
    }

    if (type == "tempoQuestion")
    {
        showTempoQuestion (msg);
        return;
    }

    if (type == "tempoClosed")
    {
        if (msg[fromID_].toString() == myRosterID && msg[textID].toString().isNotEmpty())
            log (msg[textID].toString());

        closeTempoQuestion ((int) msg[ids::id], msg[fromID_].toString(), (double) msg[bpmID]);
        return;
    }

    if (type == "snapshot")
        loadSnapshot (msg);
    else if (type == "ops")
    {
        if ((int) msg[songNumberID] == songNumber)   // (not changes to the song before)
            applyOps (msg, nullptr);
    }
    else if (type == "transport")
        applyTransport (msg);
    else if (type == "hash" && editSync != nullptr && mode == Mode::joined)
    {
        flushOutgoing();

        // Only compare once the host has everything we sent, otherwise the
        // difference is just changes still on the wire.
        if ((int) msg[ids::ack] != localSeq)
            return;

        const bool match = msg[ids::hash].toString() == juce::String (editSync->computeHash());

        if (match != lastHashMatched)
        {
            lastHashMatched = match;
            statusChanged();
        }

        if (match)
        {
            hashMismatchCount = 0;
            return;
        }

        if (++hashMismatchCount >= 2 && ! isRecording()
             && juce::Time::getMillisecondCounter() - lastResyncRequestTime > 3000)
        {
            lastResyncRequestTime = juce::Time::getMillisecondCounter();
            log ("Copies differ, asking the host for a fresh copy");
            juce::ValueTree req (ids::MSG);
            req.setProperty (ids::type, "resync", nullptr);
            from.sendTree (req);
        }
    }
}

//==============================================================================
void Session::sendLocalOp (const juce::ValueTree& op)
{
    if (mode != Mode::hosting && mode != Mode::joined)
        return;

    const bool wasEmpty = pendingOps.getNumChildren() == 0;
    pendingOps.appendChild (op.createCopy(), nullptr);

    // Batch everything changed in this message-loop turn into one message.
    if (wasEmpty)
        juce::MessageManager::callAsync ([weak = juce::WeakReference<Session> (this)]
                                        { if (weak != nullptr) weak->flushOutgoing(); });
}

void Session::flushOutgoing()
{
    if (editSync != nullptr)
        editSync->flushNow();      // turns collected edits into ops (lands in pendingOps)

    if (pendingOps.getNumChildren() == 0)
        return;

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "ops", nullptr);
    msg.setProperty (ids::seq, ++localSeq, nullptr);
    msg.setProperty (ids::peer, peerIndex, nullptr);
    msg.setProperty (songNumberID, songNumber, nullptr);

    for (int i = 0; i < pendingOps.getNumChildren(); ++i)
        msg.appendChild (pendingOps.getChild (i).createCopy(), nullptr);

    pendingOps.removeAllChildren (nullptr);

    if (mode == Mode::hosting)
        broadcast (msg, nullptr);
    else if (mode == Mode::joined && hostLink != nullptr)
        hostLink->sendTree (msg);
}

void Session::broadcast (const juce::ValueTree& msg, PeerConnection* except)
{
    // Keep ordering: our own queued changes happened first.
    if (msg[ids::type].toString() != "ops" || except != nullptr)
        flushOutgoing();

    const juce::ScopedLock sl (clients.getLock());

    for (auto* c : clients)
        if (c != except && c->welcomed && ! c->dead)
            c->sendTree (msg);
}

juce::ValueTree Session::applyOps (const juce::ValueTree& msg, PeerConnection* from)
{
    auto applied = msg.createCopy();
    applied.removeAllChildren (nullptr);

    if (editSync == nullptr)
        return applied;

    bool allOk = true;

    for (const auto& op : msg)
    {
        if (editSync->applyRemote (op))
            applied.appendChild (op.createCopy(), nullptr);
        else
            allOk = false;
    }

    if (! allOk)
    {
        if (mode == Mode::joined)
            hashMismatchCount = 2;     // ask for a fresh copy at the next hash check
        else if (from != nullptr)
            sendSnapshotTo (*from, "a change could not be applied");
    }

    if (onRemoteChange)
        onRemoteChange();

    return applied;
}

void Session::applyTransport (const juce::ValueTree& msg)
{
    // Only playback from the share this PC follows (anything else is someone's own playback).
    if (edit == nullptr || ! following || msg[sharerID].toString() != shareOwner)
        return;

    if (msg.hasProperty (ids::pattern) && compiler != nullptr)
        compiler->setPattern (msg[ids::pattern].toString());

    if (msg.hasProperty (ids::songMode) && compiler != nullptr)
        compiler->setSongMode ((bool) msg[ids::songMode]);

    if (msg.hasProperty (soloID))
    {
        sharedSolo = MixerRouter::Solo::fromString (msg[soloID].toString());
        applySolo();
    }

    auto& transport = edit->getTransport();
    const bool shouldPlay = (bool) msg[ids::playing];
    const bool recordingHere = isRecording();

    if (msg[ids::recordingBy].toString().isNotEmpty())
        log (msg[ids::recordingBy].toString() + " is recording - playing along");

    // While this PC records, nobody else can move its playhead (it would break the take).
    if (! (bool) msg[ids::patternOnly] && ! recordingHere)
        transport.setPosition (te::TimePosition::fromSeconds ((double) msg[ids::position]));

    if (shouldPlay && ! transport.isPlaying())
    {
        if (compiler != nullptr)
            compiler->compileNow();

        transport.play (false);
    }
    else if (! shouldPlay && transport.isPlaying())
    {
        transport.stop (false, false);

        if (recordingHere && compiler != nullptr)
            compiler->compileNow();   // looping back on
    }

    // The person sharing plays a pattern you're not looking at: offer to go there (once per pattern).
    const auto pattern = getPlayingPattern();

    if (! isSongMode() && pattern.isNotEmpty() && pattern != viewingPattern && pattern != lastOfferedPattern)
    {
        lastOfferedPattern = pattern;

        if (onSharedPatternOffered)
            onSharedPatternOffered (pattern, nameOf (shareOwner));
    }

    if (onTransportChanged)
        onTransportChanged();

    if (onRemoteChange)
        onRemoteChange();
}

juce::String Session::getPlayingPattern() const
{
    return compiler != nullptr ? compiler->getPattern() : juce::String();
}

void Session::sendTransport (bool patternOnly, const juce::String& recordingBy, const juce::String& to)
{
    if (edit == nullptr || ! isSharing())
        return;   // your playback is yours unless you share it

    auto& transport = edit->getTransport();

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "transport", nullptr);
    msg.setProperty (sharerID, myRosterID, nullptr);
    msg.setProperty (ids::playing, transport.isPlaying() || recordingBy.isNotEmpty(), nullptr);

    if (recordingBy.isNotEmpty())
        msg.setProperty (ids::recordingBy, recordingBy, nullptr);

    if (to.isNotEmpty())
        msg.setProperty (toID, to, nullptr);

    msg.setProperty (ids::position, transport.getPosition().inSeconds(), nullptr);
    msg.setProperty (ids::pattern, getPlayingPattern(), nullptr);
    msg.setProperty (ids::songMode, isSongMode(), nullptr);
    msg.setProperty (ids::patternOnly, patternOnly, nullptr);
    msg.setProperty (soloID, mySolo.toString(), nullptr);   // followers hear what you solo

    if (mode == Mode::hosting)
    {
        if (to.isEmpty())
        {
            broadcast (msg, nullptr);
        }
        else
        {
            flushOutgoing();
            const juce::ScopedLock sl (clients.getLock());

            for (auto* c : clients)
                if (juce::String (c->tag) == to && c->welcomed && ! c->dead)
                    c->sendTree (msg);
        }
    }
    else if (mode == Mode::joined && hostLink != nullptr)
    {
        flushOutgoing();
        hostLink->sendTree (msg);
    }
}

void Session::setPlaying (bool shouldPlay, double startSeconds)
{
    if (edit == nullptr || following)
        return;

    auto& transport = edit->getTransport();

    if (shouldPlay)
    {
        if (compiler != nullptr)
            compiler->compileNow();

        transport.setPosition (te::TimePosition::fromSeconds (startSeconds));
        transport.play (false);
    }
    else
    {
        const bool wasRecording = transport.isRecording();
        transport.stop (false, false);   // a recording becomes a take (see Recorder)
        transport.setPosition (te::TimePosition::fromSeconds (startSeconds));

        if (wasRecording && compiler != nullptr)
            compiler->compileNow();      // looping back on
    }

    sendTransport (false);

    if (onTransportChanged)
        onTransportChanged();
}

bool Session::startRecording (double startSeconds)
{
    if (edit == nullptr || isRecording() || following)
        return false;

    if (compiler != nullptr)
    {
        compiler->setSongMode (true);   // takes land in the playlist
        compiler->compileNow();
    }

    if (! recorder->start (startSeconds))
        return false;

    // People following your share play along from the same spot (in SONG mode too;
    // with a count-in, the engine has already put the playhead where the count-in starts).
    sendTransport (false, peerName);

    if (onTransportChanged)
        onTransportChanged();

    return true;
}

void Session::seek (double seconds)
{
    if (edit == nullptr || isRecording() || following)
        return;

    edit->getTransport().setPosition (te::TimePosition::fromSeconds (seconds));

    if (edit->getTransport().isPlaying())
        sendTransport (false);
}

bool Session::isSongMode() const
{
    return compiler != nullptr && compiler->isSongMode();
}

void Session::setSongMode (bool shouldBeSong)
{
    if (compiler == nullptr || following || shouldBeSong == compiler->isSongMode())
        return;

    compiler->setSongMode (shouldBeSong);
    sendTransport (true);

    if (onTransportChanged)
        onTransportChanged();
}

void Session::setPlayingPattern (const juce::String& patternSid)
{
    if (compiler == nullptr || following || patternSid == compiler->getPattern())
        return;

    compiler->setPattern (patternSid);
    sendTransport (true);

    if (onTransportChanged)
        onTransportChanged();
}

//==============================================================================
// This PC's playback, kept when the song object is replaced and restored after a share
Session::LocalPlayback Session::captureLocalPlayback() const
{
    LocalPlayback p;

    if (edit != nullptr)
    {
        p.playing = edit->getTransport().isPlaying();
        p.position = edit->getTransport().getPosition().inSeconds();
    }

    if (compiler != nullptr)
    {
        p.songMode = compiler->isSongMode();
        p.pattern = compiler->getPattern();
    }

    return p;
}

void Session::restoreLocalPlayback (const LocalPlayback& p, bool resumePlaying)
{
    if (edit == nullptr || compiler == nullptr)
        return;

    auto& transport = edit->getTransport();

    if (transport.isPlaying())
        transport.stop (false, false);

    compiler->setSongMode (p.songMode);

    if (p.pattern.isNotEmpty() && song::findPattern (*edit, p.pattern).isValid())
        compiler->setPattern (p.pattern);

    compiler->compileNow();
    transport.setPosition (te::TimePosition::fromSeconds (p.position));

    if (resumePlaying && p.playing)
        transport.play (false);

    if (onTransportChanged)
        onTransportChanged();
}

//==============================================================================
// Solo, metronome, count-in
void Session::applySolo()
{
    if (router != nullptr)
        router->setSolo (following ? sharedSolo : mySolo);

    if (onRemoteChange)
        onRemoteChange();   // the buttons show it
}

bool Session::toggleSolo (const juce::String& trackID, bool isInsert)
{
    if (following || trackID.isEmpty())
        return false;   // the person sharing decides what everyone hears

    auto& list = isInsert ? mySolo.inserts : mySolo.channels;

    if (list.contains (trackID))
        list.removeString (trackID);
    else
        list.add (trackID);

    applySolo();
    sendTransport (true);   // people following your share hear it too
    return true;
}

bool Session::isSoloed (const juce::String& trackID) const
{
    const auto& s = following ? sharedSolo : mySolo;
    return s.channels.contains (trackID) || s.inserts.contains (trackID);
}

bool Session::isSilencedBySolo (const juce::String& trackID) const
{
    return router != nullptr && router->isSilencedBySolo (trackID);
}

void Session::clearSolo()
{
    if (following || mySolo.isEmpty())
        return;

    mySolo = {};
    applySolo();
    sendTransport (true);
}

void Session::setMetronome (bool on)
{
    metronome = on;

    if (edit != nullptr)
        edit->clickTrackEnabled = on;
}

void Session::setCountInBars (int bars)     { recorder->setCountInBars (bars); }
int Session::getCountInBars() const         { return recorder->getCountInBars(); }

//==============================================================================
// Sharing playback
void Session::resetSessionOnlyState()
{
    if (following)
    {
        following = false;
        applySolo();
        restoreLocalPlayback (beforeShare, false);
    }

    shareOwner = {};
    shareListenersSeen.clearQuick();
    lastOfferedPattern = {};
    tempoVote = {};
    pendingTempo = 0.0;

    for (auto id : openTempoQuestions)
        if (onTempoQuestionClosed)
            onTempoQuestionClosed (id);

    openTempoQuestions.clearQuick();

    if (onTransportChanged)
        onTransportChanged();
}

juce::String Session::nameOf (const juce::String& rosterID) const
{
    for (auto p : roster)
        if (p[ids::id].toString() == rosterID)
            return p[ids::name].toString();

    return rosterID == myRosterID ? peerName : juce::String ("someone");
}

juce::String Session::getShareOwnerName() const
{
    return shareOwner.isNotEmpty() ? nameOf (shareOwner) : juce::String();
}

juce::Colour Session::getShareOwnerColour() const
{
    for (auto p : roster)
        if (p[ids::id].toString() == shareOwner)
            return personColour ((int) p[colourID]);

    return personColour (0);
}

juce::String Session::startSharing()
{
    if (mode != Mode::hosting && mode != Mode::joined)
        return "Sharing works when you're in a session";

    if (shareOwner.isNotEmpty())
        return shareOwner == myRosterID ? juce::String() : nameOf (shareOwner) + " is already sharing";

    if (mode == Mode::hosting)
    {
        handleShareRequest ("0", peerName, true);
    }
    else if (hostLink != nullptr)
    {
        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "shareStart", nullptr);
        hostLink->sendTree (msg);
    }

    return {};
}

void Session::stopSharing()
{
    if (! isSharing())
        return;

    if (mode == Mode::hosting)
    {
        handleShareRequest ("0", peerName, false);
    }
    else if (hostLink != nullptr)
    {
        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "shareStop", nullptr);
        hostLink->sendTree (msg);
    }
}

void Session::leaveShare()      { unfollowShare ("left"); }
void Session::joinShare()       { followShare (true); }

void Session::setFocus (bool shouldFocus)
{
    if (focus == shouldFocus)
        return;

    focus = shouldFocus;
    log (focus ? "Focus on: when someone shares their playback, you keep working on your own"
               : "Focus off");

    if (focus && following)
        unfollowShare ("focus");
    else if (! focus && isShareRunning() && ! isSharing() && ! following && ! isRecording())
        followShare (true);
    else
        sendPresence();
}

void Session::handleShareRequest (const juce::String& fromID, const juce::String& fromName, bool start)
{
    if (mode != Mode::hosting)
        return;

    if (start)
    {
        if (shareOwner.isNotEmpty())
            return;   // someone else shares already (everyone's roster says who)

        // Who won't be pulled in: people in Focus or recording.
        juce::StringArray notFollowing;

        auto check = [&notFollowing, &fromID] (const juce::String& id, const juce::String& name, bool inFocus, bool rec)
        {
            if (id == fromID)
                return;

            if (inFocus)
                notFollowing.add (name + " (Focus)");
            else if (rec)
                notFollowing.add (name + " (recording)");
        };

        check ("0", peerName, focus, isRecording());

        {
            const juce::ScopedLock sl (clients.getLock());

            for (auto* c : clients)
                if (c->welcomed && ! c->dead)
                    check (juce::String (c->tag), c->peerName, c->focus, c->recording);
        }

        broadcastSystemChat (fromName + " is sharing their playback"
                              + (notFollowing.isEmpty() ? juce::String()
                                                        : " (not following: " + notFollowing.joinIntoString (", ") + ")"));
        setShareOwner (fromID);
    }
    else if (shareOwner == fromID && fromID.isNotEmpty())
    {
        broadcastSystemChat (fromName + " stopped sharing");
        setShareOwner ({});
    }
}

void Session::setShareOwner (const juce::String& newOwner)
{
    if (newOwner == shareOwner)
        return;

    const auto old = shareOwner;
    shareOwner = newOwner;
    shareOwnerChanged (old);   // the host reacts like everyone else...
    rebuildRoster();           // ...and everyone else learns it from the roster
}

void Session::shareOwnerChanged (const juce::String& oldOwner)
{
    juce::ignoreUnused (oldOwner);
    lastOfferedPattern = {};
    shareListenersSeen.clearQuick();

    if (following)
    {
        following = false;
        applySolo();
        restoreLocalPlayback (beforeShare, false);
        log ("The shared playback ended - back to your own (where you were before)");
    }

    if (isSharing())
    {
        log ("You're sharing your playback: everyone following hears what you play. Only you control it");
    }
    else if (isShareRunning())
    {
        const auto who = nameOf (shareOwner);

        if (focus)
            log (who + " is sharing their playback. You're in Focus, so you keep working (click 'Join' to listen along)");
        else if (isRecording())
            log (who + " is sharing their playback. Join when your take is done");
        else
            followShare (false);
    }

    sendPresence();

    if (onTransportChanged)
        onTransportChanged();

    if (onPeopleChanged)
        onPeopleChanged();
}

void Session::followShare (bool manual)
{
    if (following || ! isShareRunning() || isSharing() || edit == nullptr)
        return;

    if (isRecording())
    {
        log ("Finish your take first");
        return;
    }

    beforeShare = captureLocalPlayback();
    following = true;
    lastOfferedPattern = {};
    sharedSolo = {};
    applySolo();   // what the person sharing solos (arrives with their playback)

    if (edit->getTransport().isPlaying())
        edit->getTransport().stop (false, false);   // until the shared playback arrives

    log ("Listening along with " + nameOf (shareOwner) + " (they control playback; press the share button to leave)");
    sendPresence (manual ? "joined" : "");

    if (onTransportChanged)
        onTransportChanged();
}

void Session::unfollowShare (const juce::String& note)
{
    if (! following)
        return;

    following = false;
    applySolo();
    restoreLocalPlayback (beforeShare, false);
    log ("Left " + nameOf (shareOwner) + "'s playback - back to your own (where you were before)");
    sendPresence (note);

    if (onTransportChanged)
        onTransportChanged();
}

void Session::sendShareToNewListeners()
{
    if (! isSharing())
    {
        shareListenersSeen.clearQuick();
        return;
    }

    juce::StringArray listening;

    for (auto p : roster)
        if ((bool) p[listeningID] && p[ids::id].toString() != myRosterID)
            listening.add (p[ids::id].toString());

    for (auto& id : listening)
        if (! shareListenersSeen.contains (id))
            sendTransport (false, {}, id);   // they just started following: give them the playback

    shareListenersSeen = listening;          // someone who leaves and comes back gets it again
}

void Session::handleShareNote (const juce::String& name, const juce::String& note)
{
    if (mode != Mode::hosting || ! isShareRunning())
        return;

    const auto owner = nameOf (shareOwner);

    if (note == "left")         broadcastSystemChat (name + " left " + owner + "'s playback");
    else if (note == "focus")   broadcastSystemChat (name + " switched to Focus and left " + owner + "'s playback");
    else if (note == "joined")  broadcastSystemChat (name + " joined " + owner + "'s playback");
}

//==============================================================================
// Tempo changes need everyone's OK
static juce::String bpmText (double bpm)
{
    return std::abs (bpm - std::round (bpm)) < 0.005 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 2);
}

bool Session::tempoNeedsApproval() const
{
    if (mode == Mode::hosting)
        return getNumConnectedPeers() > 0;

    return mode == Mode::joined;
}

void Session::applyTempo (double bpm)
{
    if (edit == nullptr)
        return;

    // Straight into the song, without the undo history: Ctrl+Z must not skip the approval.
    if (auto* tempo = edit->tempoSequence.getTempo (0))
        tempo->state.setProperty (te::IDs::bpm, juce::jlimit (40.0, 250.0, bpm), nullptr);
}

void Session::requestTempo (double bpmIn)
{
    if (edit == nullptr)
        return;

    auto* tempo = edit->tempoSequence.getTempo (0);

    if (tempo == nullptr)
        return;

    const double bpm = juce::jlimit (40.0, 250.0, std::round (bpmIn * 100.0) / 100.0);

    if (! tempoNeedsApproval())
    {
        pendingTempo = 0.0;
        applyTempo (bpm);
        statusChanged();
        return;
    }

    const bool same = std::abs (tempo->getBpm() - bpm) < 0.001;

    if (same && pendingTempo == 0.0)
        return;

    pendingTempo = same ? 0.0 : bpm;   // back to the current tempo = withdraw the request

    if (mode == Mode::hosting)
    {
        startTempoVote ("0", peerName, bpm);
    }
    else if (hostLink != nullptr)
    {
        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "tempoAsk", nullptr);
        msg.setProperty (bpmID, bpm, nullptr);
        hostLink->sendTree (msg);
    }

    statusChanged();
}

void Session::startTempoVote (const juce::String& fromID, const juce::String& fromName, double bpm)
{
    if (mode != Mode::hosting || edit == nullptr)
        return;

    auto* tempo = edit->tempoSequence.getTempo (0);

    if (tempo == nullptr)
        return;

    bpm = juce::jlimit (40.0, 250.0, bpm);
    const double current = tempo->getBpm();

    auto tellRequester = [this, &fromID, &bpm] (const juce::String& result, const juce::String& text)
    {
        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "tempoClosed", nullptr);
        msg.setProperty (ids::id, 0, nullptr);
        msg.setProperty (fromID_, fromID, nullptr);
        msg.setProperty (bpmID, bpm, nullptr);
        msg.setProperty (resultID, result, nullptr);
        msg.setProperty (textID, text, nullptr);

        if (fromID == "0")
        {
            if (text.isNotEmpty())
                log (text);

            closeTempoQuestion (0, "0", bpm);
            return;
        }

        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
            if (juce::String (c->tag) == fromID && ! c->dead)
                c->sendTree (msg);
    };

    if (tempoVote.id != 0)
    {
        if (tempoVote.fromID != fromID)
        {
            tellRequester ("busy", tempoVote.fromName + "'s tempo change (" + bpmText (tempoVote.bpm)
                                     + " BPM) is still waiting for answers. Try again in a moment");
            return;
        }

        // The same person changed their mind: the old question goes away.
        finishTempoVote ("replaced", std::abs (bpm - current) < 0.001 ? fromName + " withdrew the tempo change"
                                                                     : juce::String());
    }

    if (std::abs (bpm - current) < 0.001)
    {
        tellRequester ("same", {});
        return;
    }

    TempoVote v;
    v.id = nextTempoID++;
    v.bpm = bpm;
    v.oldBpm = current;
    v.fromID = fromID;
    v.fromName = fromName;
    v.deadline = juce::Time::getMillisecondCounter() + (juce::uint32) tempoAnswerTimeoutMs;

    if (fromID != "0")
        v.waiting.add ("0");

    {
        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
            if (c->welcomed && ! c->dead && juce::String (c->tag) != fromID)
                v.waiting.add (juce::String (c->tag));
    }

    if (v.waiting.isEmpty())
    {
        applyTempo (bpm);
        tellRequester ("approved", {});
        return;
    }

    tempoVote = v;
    broadcastSystemChat (fromName + " asks to change the tempo from " + bpmText (current) + " to " + bpmText (bpm) + " BPM");

    juce::ValueTree q (ids::MSG);
    q.setProperty (ids::type, "tempoQuestion", nullptr);
    q.setProperty (ids::id, v.id, nullptr);
    q.setProperty (bpmID, bpm, nullptr);
    q.setProperty (oldBpmID, current, nullptr);
    q.setProperty (ids::name, fromName, nullptr);
    q.setProperty (fromID_, fromID, nullptr);

    for (auto& id : v.waiting)
    {
        if (id == "0")
        {
            showTempoQuestion (q);
            continue;
        }

        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
            if (juce::String (c->tag) == id && ! c->dead)
                c->sendTree (q);
    }
}

void Session::handleTempoAnswer (int id, const juce::String& fromID, bool yes)
{
    if (mode != Mode::hosting || tempoVote.id == 0 || id != tempoVote.id || ! tempoVote.waiting.contains (fromID))
        return;

    if (! yes)
    {
        finishTempoVote ("declined", nameOf (fromID) + " declined the tempo change to " + bpmText (tempoVote.bpm)
                                       + " BPM - it stays at " + bpmText (tempoVote.oldBpm));
        return;
    }

    tempoVote.waiting.removeString (fromID);
    checkTempoVote();
}

void Session::checkTempoVote()
{
    if (mode != Mode::hosting || tempoVote.id == 0)
        return;

    auto connected = [this] (const juce::String& id)
    {
        if (id == "0")
            return true;

        const juce::ScopedLock sl (clients.getLock());

        for (auto* c : clients)
            if (juce::String (c->tag) == id && c->welcomed && ! c->dead)
                return true;

        return false;
    };

    if (! connected (tempoVote.fromID))
    {
        finishTempoVote ("cancelled", tempoVote.fromName + " left, so their tempo change is cancelled");
        return;
    }

    for (int i = tempoVote.waiting.size(); --i >= 0;)
        if (! connected (tempoVote.waiting[i]))
            tempoVote.waiting.remove (i);   // people who left don't hold it up

    if (tempoVote.waiting.isEmpty())
    {
        finishTempoVote ("approved", "Tempo changed to " + bpmText (tempoVote.bpm) + " BPM (everyone agreed)");
        return;
    }

    if ((int) (juce::Time::getMillisecondCounter() - tempoVote.deadline) >= 0)
    {
        juce::StringArray names;

        for (auto& id : tempoVote.waiting)
            names.add (nameOf (id));

        finishTempoVote ("expired", "Tempo change to " + bpmText (tempoVote.bpm) + " BPM cancelled: "
                                      + names.joinIntoString (", ") + " didn't answer in time");
    }
}

void Session::finishTempoVote (const juce::String& result, const juce::String& chatText)
{
    if (tempoVote.id == 0)
        return;

    const auto v = tempoVote;
    tempoVote = {};

    if (result == "approved")
        applyTempo (v.bpm);

    if (chatText.isNotEmpty())
        broadcastSystemChat (chatText);

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "tempoClosed", nullptr);
    msg.setProperty (ids::id, v.id, nullptr);
    msg.setProperty (fromID_, v.fromID, nullptr);
    msg.setProperty (bpmID, v.bpm, nullptr);
    msg.setProperty (resultID, result, nullptr);
    broadcast (msg, nullptr);
    closeTempoQuestion (v.id, v.fromID, v.bpm);
}

void Session::showTempoQuestion (const juce::ValueTree& msg)
{
    TempoQuestion q;
    q.id = (int) msg[ids::id];
    q.bpm = msg[bpmID];
    q.oldBpm = msg[oldBpmID];
    q.fromName = msg[ids::name].toString();
    openTempoQuestions.addIfNotAlreadyThere (q.id);
    log (q.fromName + " asks to change the tempo from " + bpmText (q.oldBpm) + " to " + bpmText (q.bpm) + " BPM");

    if (onTempoQuestion)
        onTempoQuestion (q);
}

void Session::closeTempoQuestion (int id, const juce::String& requesterID, double bpm)
{
    if (openTempoQuestions.contains (id))
    {
        openTempoQuestions.removeFirstMatchingValue (id);

        if (onTempoQuestionClosed)
            onTempoQuestionClosed (id);
    }

    // Only the answer to my current request (an older one I replaced doesn't count).
    if (requesterID == myRosterID && pendingTempo != 0.0 && std::abs (pendingTempo - bpm) < 0.001)
    {
        pendingTempo = 0.0;
        statusChanged();
    }
}

void Session::answerTempo (int id, bool yes)
{
    openTempoQuestions.removeFirstMatchingValue (id);

    if (mode == Mode::hosting)
    {
        handleTempoAnswer (id, "0", yes);
    }
    else if (mode == Mode::joined && hostLink != nullptr)
    {
        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "tempoAnswer", nullptr);
        msg.setProperty (ids::id, id, nullptr);
        msg.setProperty (yesID, yes, nullptr);
        hostLink->sendTree (msg);
    }
}

//==============================================================================
void Session::sendSnapshotTo (PeerConnection& c, const juce::String& reason, const juce::ValueTree& premade)
{
    if (editSync == nullptr)
        return;

    flushOutgoing();

    // A fresh peer number on every snapshot keeps that peer's new IDs above
    // every ID already in the song (see installEdit) - also above IDs made by
    // people in earlier sessions, when the song was opened from a file.
    uint64_t highestID = 0;

    for (auto& id : te::EditItemID::findAllIDs (edit->state))
        highestID = juce::jmax (highestID, id.getRawID());

    nextPeerIndex = juce::jmax (nextPeerIndex, (int) (highestID / (uint64_t) idRangePerPeer) + 1);
    c.peerIndex = nextPeerIndex++;
    c.welcomed = true;

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "snapshot", nullptr);
    msg.setProperty (ids::peer, c.peerIndex, nullptr);
    msg.setProperty (ids::reason, reason, nullptr);
    msg.setProperty (songNumberID, songNumber, nullptr);
    msg.setProperty (ids::pattern, getPlayingPattern(), nullptr);   // a starting pattern for newcomers

    if (premade.isValid())
    {
        msg.appendChild (premade.createCopy(), nullptr);
        c.sendTree (msg);
        return;
    }

    msg.appendChild (editSync->createSnapshot(), nullptr);
    c.sendTree (msg);
    flushOutgoing();   // plugin states written by the snapshot go to everyone else
}

//==============================================================================
// Song files
juce::String Session::getSongTitle() const
{
    return edit != nullptr ? song::getTitle (*edit) : juce::String();
}

juce::String Session::findTailscaleAddress()
{
    for (auto& a : juce::IPAddress::getAllAddresses (false))
        if (a.address[0] == 100 && a.address[1] >= 64 && a.address[1] <= 127)
            return a.toString();

    return {};
}

juce::String Session::replaceSong (std::unique_ptr<te::Edit> newEdit, const juce::String& announcement)
{
    if (newEdit == nullptr)
        return "Could not open the song";

    deferredSnapshot = {};
    lastReplaceReason = "own";

    // A new song ends any shared playback (its patterns are gone).
    if (mode == Mode::hosting && isShareRunning())
    {
        broadcastSystemChat ("Shared playback ended (new song)");
        setShareOwner ({});
    }

    const bool wasSongMode = isSongMode();   // PAT / SONG is yours: it stays as it was

    if (edit != nullptr && edit->getTransport().isPlaying())
        edit->getTransport().stop (false, false);

    installEdit (std::move (newEdit), true);
    compiler->setSongMode (wasSongMode);
    compiler->compileNow();
    ++songNumber;   // changes still on their way for the old song won't touch this one

    if (mode == Mode::hosting)
    {
        // One snapshot of the new song for everyone. Nothing about the new song may be
        // sent before it (the others would apply it to the old song), so the changes
        // collected while building it are dropped: they're all in the snapshot.
        auto snapshot = editSync->createSnapshot();
        pendingOps.removeAllChildren (nullptr);

        juce::Array<PeerConnection*> targets;

        {
            const juce::ScopedLock sl (clients.getLock());

            for (auto* c : clients)
                if (c->welcomed && ! c->dead)
                    targets.add (c);
        }

        for (auto* c : targets)
            sendSnapshotTo (*c, "newSong", snapshot);

        rebuildRoster();
        broadcastSystemChat (peerName + " " + announcement);
    }

    statusChanged();
    return {};
}

juce::String Session::newProject()
{
    if (! canReplaceSong())
        return "Only the host can start a new song while you're connected";

    if (isRecording())
        return "Stop recording first";

    auto newEdit = te::createEmptyEdit (engine, juce::File());
    song::createDefaultSong (*newEdit);
    song::setTitle (*newEdit, "Untitled");
    return replaceSong (std::move (newEdit), "started a new song");
}

juce::String Session::newProject (std::function<void (te::Edit&)> fill, const juce::String& title)
{
    if (! canReplaceSong())
        return "Only the host can start a new song while you're connected";

    if (isRecording())
        return "Stop recording first";

    auto newEdit = te::createEmptyEdit (engine, juce::File());

    // Tracktion starts an Edit with one plain audio track; songs only use channels and inserts.
    for (auto* t : te::getAudioTracks (*newEdit))
        newEdit->deleteTrack (t);

    if (fill)
        fill (*newEdit);

    song::setTitle (*newEdit, title);
    newEdit->getUndoManager().clearUndoHistory();
    return replaceSong (std::move (newEdit), "started a new song");
}

void Session::openProject (const juce::File& file, std::function<void (juce::String)> onDone,
                           const juce::String& titleOverride)
{
    if (! canReplaceSong())
    {
        onDone ("Only the host can open a song while you're connected (you can still save your copy)");
        return;
    }

    if (isRecording())
    {
        onDone ("Stop recording first");
        return;
    }

    ++fileJobsRunning;
    juce::WeakReference<Session> weak (this);

    fileJobs.addJob ([this, weak, file, onDone, titleOverride]
    {
        auto result = project::load (file, assets);   // reads the file, restores its audio (background)

        juce::MessageManager::callAsync ([weak, result, file, onDone, titleOverride]
        {
            if (weak == nullptr)
                return;

            auto& self = *weak;
            --self.fileJobsRunning;

            if (! result.ok())
            {
                onDone (result.error);
                return;
            }

            if (! self.canReplaceSong() || self.isRecording())
            {
                onDone ("Couldn't open it now (you joined a session or started recording meanwhile)");
                return;
            }

            // The file name is the song's name (set before loading, so it's simply part of the song).
            auto state = result.song.createCopy();
            // (A song started from a template gets its own name instead.)
            const auto title = titleOverride.isNotEmpty() ? titleOverride : file.getFileNameWithoutExtension();

            for (auto child : state)
                if (child.hasType (song::ids::SONGMODEL))
                    child.setProperty (song::ids::title, title, nullptr);

            const auto error = self.replaceSong (te::loadEditFromState (self.engine, state),
                                                 "opened \"" + title + "\"");

            if (error.isEmpty())
            {
                self.log ("Opened \"" + title + "\"" + (result.audioRestored > 0 ? " (" + juce::String (result.audioRestored)
                                                                                     + " audio files restored)" : juce::String()));

                if (result.audioDamaged > 0)
                    self.log (juce::String (result.audioDamaged) + " audio file(s) in it were damaged and were left out; "
                              "they'll be fetched from the others if someone has them");
            }

            onDone (error);
        });
    });
}

void Session::saveProject (const juce::File& file, bool includeAudio, std::function<void (project::SaveResult)> onDone,
                           bool asBackup)
{
    if (edit == nullptr || editSync == nullptr)
    {
        project::SaveResult r;
        r.error = "There is no song to save";
        onDone (r);
        return;
    }

    auto song = editSync->createSnapshot();   // also writes the plugins' current settings into it
    const auto songHash = editSync->computeContentHash();   // what "unsaved changes" compares with
    // Every audio file with the name the song gives it (for readable file names in the project folder).
    std::vector<project::AudioFile> audio;

    for (auto& hash : song::getAssetHashes (*edit))
        audio.push_back ({ hash, song::getAssetName (*edit, hash), song::isTakeAsset (*edit, hash) });

    ++fileJobsRunning;
    juce::WeakReference<Session> weak (this);

    fileJobs.addJob ([this, weak, file, song, audio, includeAudio, by = peerName, onDone, songHash, asBackup]
    {
        const auto folder = file.getParentDirectory();
        auto result = asBackup ? project::save (file, song, assets, audio, true, by, backups::knownAudio (folder), false)
                               : project::save (file, song, assets, audio, includeAudio, by);
        result.songHash = songHash;

        if (asBackup && result.ok())
            backups::prune (folder);

        juce::MessageManager::callAsync ([weak, result, onDone]
        {
            if (weak != nullptr)
                --weak->fileJobsRunning;

            onDone (result);
        });
    });
}

void Session::loadSnapshot (const juce::ValueTree& msg)
{
    if (msg.getNumChildren() != 1)
        return;

    auto state = msg.getChild (0).createCopy();
    const auto reason = msg[ids::reason].toString();

    // Replacing the song object would end a recording: wait until the take is saved.
    if (reason != "join" && (isRecording() || recorder->isBusy()))
    {
        if (! deferredSnapshot.isValid())
            log ("A fresh copy of the song arrived; it will be applied once your take is saved");

        deferredSnapshot = msg.createCopy();
        return;
    }

    if (reason == "newSong")
    {
        log ("The host switched to another song");
    }
    else if (reason != "join" && edit != nullptr)
    {
        ++resyncCount;
        log ("Fresh copy received (" + reason + "). First difference was at "
              + EditSync::describeFirstDifference (edit->state, state));
    }

    songNumber = (int) msg[songNumberID];

    // A fresh copy of the song we already have: merge it in place. Only what differs
    // changes, so the plugins stay loaded (reloading the whole song reloads every
    // plugin - seconds of freeze with big instruments). If the copies still differ
    // afterwards, reload it as before.
    if (reason != "join" && reason != "newSong" && edit != nullptr && editSync != nullptr)
    {
        pendingOps.removeAllChildren (nullptr);

        if (editSync->applySnapshotInPlace (state))
        {
            pendingOps.removeAllChildren (nullptr);
            ++inPlaceResyncs;
            hashMismatchCount = 0;
            lastHashMatched = true;
            lastReplaceReason = reason;
            compiler->compileNow();
            log ("Fresh copy merged into your song (no reload)");
            statusChanged();
            return;
        }

        log ("Merging the fresh copy left a difference; reloading the song");
    }

    peerIndex = (int) msg[ids::peer];

    juce::ValueTree local (ids::SYNCLOCAL);
    local.setProperty (te::IDs::id, juce::String (idRangePerPeer * peerIndex), nullptr);
    state.appendChild (local, nullptr);

    auto newEdit = te::loadEditFromState (engine, state);

    if (newEdit == nullptr)
    {
        log ("Could not open the song sent by the host");
        return;
    }

    pendingOps.removeAllChildren (nullptr);
    hashMismatchCount = 0;
    lastHashMatched = true;
    mode = Mode::joined;
    lastReplaceReason = reason;

    // Playback is this PC's own: a fresh copy of the same song keeps it going where it was;
    // a different song (joining, or the host switched songs) starts stopped at the beginning.
    const bool sameSong = edit != nullptr && reason != "join" && reason != "newSong";
    const auto playback = captureLocalPlayback();

    if (edit != nullptr && edit->getTransport().isPlaying())
        edit->getTransport().stop (false, false);

    installEdit (std::move (newEdit));

    if (sameSong)
    {
        restoreLocalPlayback (playback, true);
    }
    else
    {
        compiler->setSongMode (playback.songMode);

        if (msg.hasProperty (ids::pattern))
            compiler->setPattern (msg[ids::pattern].toString());

        compiler->compileNow();
    }

    if (reason == "join")
    {
        log ("Got the song. You are peer " + juce::String (peerIndex));
        sendPresence();   // which pattern I'm looking at
    }

    statusChanged();
}

//==============================================================================
void Session::timerCallback()
{
    ++tick;

    // Drop connections that went away.
    {
        const juce::ScopedLock sl (clients.getLock());

        for (int i = clients.size(); --i >= 0;)
        {
            if (clients[i]->dead)
            {
                for (auto& [hash, tags] : assetWantedBy)
                    tags.removeAllInstancesOf (clients[i]->tag);

                clients.remove (i);
            }
        }
    }

    if (tick % 2 == 0)
        checkAssets();

    if (isRecording() != presenceRecording)
    {
        presenceRecording = isRecording();
        sendPresence();   // the others see a recording dot next to your name

        if (presenceRecording && mode == Mode::hosting)
            broadcastSystemChat (peerName + " started recording");
    }

    checkTempoVote();

    if (deferredSnapshot.isValid() && ! isRecording() && ! recorder->isBusy())
    {
        auto msg = deferredSnapshot;
        deferredSnapshot = {};
        loadSnapshot (msg);
    }

    if (tick % 3 != 0 || mode != Mode::hosting || editSync == nullptr)
        return;

    flushOutgoing();
    const auto hash = juce::String (editSync->computeHash());

    const juce::ScopedLock sl (clients.getLock());

    for (auto* c : clients)
    {
        if (! c->welcomed || c->dead)
            continue;

        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "hash", nullptr);
        msg.setProperty (ids::hash, hash, nullptr);
        msg.setProperty (ids::ack, c->lastSeqApplied, nullptr);
        c->sendTree (msg);
    }
}

//==============================================================================
// Audio files: anyone missing a file asks (joiners ask the host, the host asks
// the joiners one by one); whoever has it sends it in chunks.

PeerConnection* Session::findLink (int tag) const
{
    if (hostLink != nullptr && hostLink->tag == tag)
        return hostLink.get();

    const juce::ScopedLock sl (clients.getLock());

    for (auto* c : clients)
        if (c->tag == tag && ! c->dead)
            return c;

    return nullptr;
}

juce::String Session::assetName (const juce::String& hash) const
{
    if (edit != nullptr)
    {
        auto name = song::getAssetName (*edit, hash);

        if (name.isNotEmpty())
            return name;
    }

    return "an audio file";
}

void Session::checkAssets()
{
    if (edit == nullptr || (mode != Mode::hosting && mode != Mode::joined))
        return;

    assets.dropStalledDownloads (15000);
    const auto now = juce::Time::getMillisecondCounter();

    for (auto& hash : song::getAssetHashes (*edit))
    {
        if (assets.has (hash) || assets.isDownloading (hash))
            continue;

        auto& last = lastAssetRequest[hash];

        if (last != 0 && now - last < 3000)
            continue;

        last = now;

        juce::ValueTree msg (ids::MSG);
        msg.setProperty (ids::type, "assetGet", nullptr);
        msg.setProperty (ids::hash, hash, nullptr);

        if (mode == Mode::joined && hostLink != nullptr)
        {
            hostLink->sendTree (msg);
        }
        else if (mode == Mode::hosting)
        {
            juce::Array<PeerConnection*> candidates;

            {
                const juce::ScopedLock sl (clients.getLock());

                for (auto* c : clients)
                    if (c->welcomed && ! c->dead)
                        candidates.add (c);
            }

            if (! candidates.isEmpty())
                candidates[assetRequestRound++ % candidates.size()]->sendTree (msg);
        }
    }
}

void Session::handleAssetMessage (PeerConnection& from, const juce::ValueTree& msg)
{
    const auto hash = msg[ids::hash].toString();

    if (msg[ids::type].toString() == "assetGet")
    {
        if (assets.has (hash))
            from.queueAssetSend (hash, assets.getFile (hash));
        else if (mode == Mode::hosting)
            assetWantedBy[hash].addIfNotAlreadyThere (from.tag);   // served once it arrives here

        return;
    }

    const juce::int64 offset = msg["offset"], total = msg["total"];
    const auto* data = msg["data"].getBinaryData();

    if (data == nullptr)
        return;

    if (offset == 0 && ! assets.has (hash) && ! assets.isDownloading (hash))
        log ("Receiving " + assetName (hash) + " (" + juce::File::descriptionOfSizeInBytes (total) + ")");

    auto result = assets.receiveChunk (hash, offset, total, *data, from.tag);

    if (result == AssetStore::ChunkResult::failed)
    {
        log (assetName (hash) + " arrived damaged; asking for it again");
        lastAssetRequest[hash] = 0;
    }
    else if (result == AssetStore::ChunkResult::accepted && onRemoteChange
              && (offset / PeerConnection::assetChunkBytes) % 16 == 0)
    {
        onRemoteChange();   // let the UI show progress now and then
    }
}

void Session::handleAssetAdded (const juce::String& hash)
{
    log ("Received " + assetName (hash));

    // Serve anyone who asked the host before it had the file.
    auto it = assetWantedBy.find (hash);

    if (it != assetWantedBy.end())
    {
        for (auto tag : it->second)
            if (auto* link = findLink (tag))
                link->queueAssetSend (hash, assets.getFile (hash));

        assetWantedBy.erase (it);
    }

    if (compiler != nullptr)
        compiler->compileNow();

    if (onRemoteChange)
        onRemoteChange();
}

} // namespace collab

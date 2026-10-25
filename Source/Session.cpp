#include "Session.h"
#include "SongModel.h"
#include "PatternCompiler.h"
#include "MixerRouter.h"

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
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("SyncTest").getChildFile ("Assets");
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
    stopTimer();
    leave();
    recorder.reset();   // detaches from the Edit while it still exists
    router.reset();
    compiler.reset();
    editSync.reset();
    edit.reset();
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
        case Mode::offline:     return "Offline";
        case Mode::hosting:     return "Hosting - " + juce::String (getNumConnectedPeers()) + " peer(s) connected";
        case Mode::connecting:  return "Connecting...";
        case Mode::joined:      return juce::String ("Joined as peer ") + juce::String (peerIndex)
                                       + (lastHashMatched ? " - in sync" : " - checking sync...");
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
    auto newEdit = te::createEmptyEdit (engine, juce::File());
    song::createDefaultSong (*newEdit);
    installEdit (std::move (newEdit));
    editSync->assignMissingSyncIDs();
}

void Session::installEdit (std::unique_ptr<te::Edit> newEdit)
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

    if (mode != Mode::offline)
    {
        mode = Mode::offline;
        log ("Left the session");
        statusChanged();
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
        mode = Mode::offline;
        juce::MessageManager::callAsync ([weak = juce::WeakReference<Session> (this)]
                                        { if (weak != nullptr) weak->hostLink.reset(); });
    }
    else
    {
        log ((c.peerName.isNotEmpty() ? c.peerName : juce::String ("A peer")) + " left");
    }

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
                log ("Rejected " + msg[ids::peerName].toString() + ": different SyncTest version");
                from.disconnect();
                return;
            }

            from.peerName = msg[ids::peerName].toString();
            sendSnapshotTo (from, "join");
            log (from.peerName + " joined as peer " + juce::String (from.peerIndex));
            statusChanged();
        }
        else if (type == "ops")
        {
            from.lastSeqApplied = (int) msg[ids::seq];
            applyOps (msg, &from);
            broadcast (msg, &from);     // relay to everyone else
        }
        else if (type == "transport")
        {
            applyTransport (msg);
            broadcast (msg, &from);
        }
        else if (type == "resync")
        {
            sendSnapshotTo (from, "resync");
        }

        return;
    }

    // Joining side
    if (type == "snapshot")
        loadSnapshot (msg);
    else if (type == "ops")
        applyOps (msg, nullptr);
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

void Session::applyOps (const juce::ValueTree& msg, PeerConnection* from)
{
    if (editSync == nullptr)
        return;

    bool allOk = true;

    for (const auto& op : msg)
        allOk = editSync->applyRemote (op) && allOk;

    if (! allOk)
    {
        if (mode == Mode::joined)
            hashMismatchCount = 2;     // ask for a fresh copy at the next hash check
        else if (from != nullptr)
            sendSnapshotTo (*from, "a change could not be applied");
    }

    if (onRemoteChange)
        onRemoteChange();
}

void Session::applyTransport (const juce::ValueTree& msg)
{
    if (edit == nullptr)
        return;

    if (msg.hasProperty (ids::pattern) && compiler != nullptr)
        compiler->setPattern (msg[ids::pattern].toString());

    if (msg.hasProperty (ids::songMode) && compiler != nullptr)
        compiler->setSongMode ((bool) msg[ids::songMode]);

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
        transport.play (false);
    }
    else if (! shouldPlay && transport.isPlaying())
    {
        transport.stop (false, false);   // a shared Stop also ends a take here (it is kept)

        if (recordingHere)
        {
            log ("Someone pressed Stop - saving your take");

            if (compiler != nullptr)
                compiler->compileNow();   // looping back on
        }
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

void Session::sendTransport (bool patternOnly, const juce::String& recordingBy)
{
    if (edit == nullptr)
        return;

    auto& transport = edit->getTransport();

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "transport", nullptr);
    msg.setProperty (ids::playing, transport.isPlaying() || recordingBy.isNotEmpty(), nullptr);

    if (recordingBy.isNotEmpty())
        msg.setProperty (ids::recordingBy, recordingBy, nullptr);

    msg.setProperty (ids::position, transport.getPosition().inSeconds(), nullptr);
    msg.setProperty (ids::pattern, getPlayingPattern(), nullptr);
    msg.setProperty (ids::songMode, isSongMode(), nullptr);
    msg.setProperty (ids::patternOnly, patternOnly, nullptr);

    if (mode == Mode::hosting)
        broadcast (msg, nullptr);
    else if (mode == Mode::joined && hostLink != nullptr)
    {
        flushOutgoing();
        hostLink->sendTree (msg);
    }
}

void Session::setPlaying (bool shouldPlay, double startSeconds)
{
    if (edit == nullptr)
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
    if (edit == nullptr || isRecording())
        return false;

    if (compiler != nullptr)
    {
        compiler->setSongMode (true);   // takes land in the playlist
        compiler->compileNow();
    }

    if (! recorder->start (startSeconds))
        return false;

    // Everyone else plays along from the same spot (and switches to SONG too).
    sendTransport (false, peerName);

    if (onTransportChanged)
        onTransportChanged();

    return true;
}

void Session::seek (double seconds)
{
    if (edit == nullptr || isRecording())
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
    if (compiler == nullptr || shouldBeSong == compiler->isSongMode())
        return;

    compiler->setSongMode (shouldBeSong);
    sendTransport (true);

    if (onTransportChanged)
        onTransportChanged();
}

void Session::setPlayingPattern (const juce::String& patternSid)
{
    if (compiler == nullptr || patternSid == compiler->getPattern())
        return;

    compiler->setPattern (patternSid);
    sendTransport (true);   // everyone hears the same pattern

    if (onTransportChanged)
        onTransportChanged();
}

//==============================================================================
void Session::sendSnapshotTo (PeerConnection& c, const juce::String& reason)
{
    if (editSync == nullptr)
        return;

    flushOutgoing();

    // A fresh peer number on every snapshot keeps that peer's new IDs above
    // every ID already in the song (see installEdit).
    c.peerIndex = nextPeerIndex++;
    c.welcomed = true;

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "snapshot", nullptr);
    msg.setProperty (ids::peer, c.peerIndex, nullptr);
    msg.setProperty (ids::reason, reason, nullptr);
    msg.setProperty (ids::playing, edit->getTransport().isPlaying(), nullptr);
    msg.setProperty (ids::position, edit->getTransport().getPosition().inSeconds(), nullptr);
    msg.setProperty (ids::pattern, getPlayingPattern(), nullptr);
    msg.setProperty (ids::songMode, isSongMode(), nullptr);
    msg.appendChild (editSync->createSnapshot(), nullptr);
    c.sendTree (msg);
    flushOutgoing();   // plugin states written by the snapshot go to everyone else
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

    if (reason != "join" && edit != nullptr)
    {
        ++resyncCount;
        log ("Fresh copy received (" + reason + "). First difference was at "
              + EditSync::describeFirstDifference (edit->state, state));
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
    installEdit (std::move (newEdit));

    if (msg.hasProperty (ids::pattern))
        compiler->setPattern (msg[ids::pattern].toString());

    if (msg.hasProperty (ids::songMode))
        compiler->setSongMode ((bool) msg[ids::songMode]);

    if ((bool) msg[ids::playing])
        applyTransport (msg);

    if (reason == "join")
        log ("Got the song. You are peer " + juce::String (peerIndex));

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

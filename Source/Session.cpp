#include "Session.h"

namespace collab
{

//==============================================================================
static juce::MemoryBlock toBlock (const juce::ValueTree& v)
{
    juce::MemoryOutputStream out;
    v.writeToStream (out);
    return out.getMemoryBlock();
}

PeerConnection::PeerConnection (Session& s)
    : InterprocessConnection (true, Session::magicNumber), session (s)
{
}

PeerConnection::~PeerConnection()
{
    disconnect();
}

void PeerConnection::sendTree (const juce::ValueTree& msg)
{
    if (! dead)
        sendMessage (toBlock (msg));
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
Session::Session (te::Engine& e) : engine (e)
{
    startTimer (1500);
}

Session::~Session()
{
    stopTimer();
    leave();
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
    // Loop the 1-bar pattern, FL-style pattern mode.
    auto& transport = edit.getTransport();
    transport.setLoopRange (edit.tempoSequence.toTime ({ te::BeatPosition::fromBeats (0.0),
                                                        te::BeatPosition::fromBeats (4.0) }));
    transport.looping = true;
    edit.playInStopEnabled = true;
    transport.ensureContextAllocated();
}

void addStarterContent (te::Edit& edit)
{
    edit.ensureNumberOfAudioTracks (1);
    auto track = te::getAudioTracks (edit)[0];
    track->setName ("Synth");

    if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
        track->pluginList.insertPlugin (synth, 0, nullptr);

    auto range = edit.tempoSequence.toTime ({ te::BeatPosition::fromBeats (0.0), te::BeatPosition::fromBeats (4.0) });

    if (auto clip = track->insertMIDIClip ("Pattern", range, nullptr))
    {
        auto& seq = clip->getSequence();

        for (int step : { 0, 4, 8, 12 })
            seq.addNote (60, te::BeatPosition::fromBeats (step * 0.25), te::BeatDuration::fromBeats (0.25), 100, 0, nullptr);
    }
}

void Session::createLocalEdit()
{
    auto newEdit = te::createEmptyEdit (engine, juce::File());
    addStarterContent (*newEdit);
    installEdit (std::move (newEdit));
    editSync->assignMissingSyncIDs();
}

void Session::installEdit (std::unique_ptr<te::Edit> newEdit)
{
    if (onEditAboutToBeReplaced)
        onEditAboutToBeReplaced();

    editSync.reset();
    edit.reset();
    edit = std::move (newEdit);

    // Fix this Edit's ID counter now, inside our own range, before any remote
    // items arrive (Tracktion picks "highest existing ID + 1" the first time).
    edit->createNewItemID();

    setUpLocalTransport (*edit);
    attachSync();

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

        if (++hashMismatchCount >= 2
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

    auto& transport = edit->getTransport();
    transport.setPosition (te::TimePosition::fromSeconds ((double) msg[ids::position]));

    if ((bool) msg[ids::playing])
        transport.play (false);
    else
        transport.stop (false, false);

    if (onRemoteChange)
        onRemoteChange();
}

void Session::setPlaying (bool shouldPlay)
{
    if (edit == nullptr)
        return;

    auto& transport = edit->getTransport();

    if (shouldPlay)
    {
        transport.setPosition (te::TimePosition());
        transport.play (false);
    }
    else
    {
        transport.stop (false, false);
    }

    juce::ValueTree msg (ids::MSG);
    msg.setProperty (ids::type, "transport", nullptr);
    msg.setProperty (ids::playing, shouldPlay, nullptr);
    msg.setProperty (ids::position, transport.getPosition().inSeconds(), nullptr);

    if (mode == Mode::hosting)
        broadcast (msg, nullptr);
    else if (mode == Mode::joined && hostLink != nullptr)
    {
        flushOutgoing();
        hostLink->sendTree (msg);
    }
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

    if ((bool) msg[ids::playing])
        applyTransport (msg);

    if (reason == "join")
        log ("Got the song. You are peer " + juce::String (peerIndex));

    statusChanged();
}

//==============================================================================
void Session::timerCallback()
{
    // Drop connections that went away.
    {
        const juce::ScopedLock sl (clients.getLock());

        for (int i = clients.size(); --i >= 0;)
            if (clients[i]->dead)
                clients.remove (i);
    }

    if (mode != Mode::hosting || editSync == nullptr)
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

} // namespace collab

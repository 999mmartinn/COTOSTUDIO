#include "Recorder.h"
#include "SongModel.h"

//==============================================================================
// Where each Edit's raw takes go (the engine asks through AppEngineBehaviour).
static juce::CriticalSection& folderLock()
{
    static juce::CriticalSection lock;
    return lock;
}

static std::map<te::Edit*, juce::File>& recordingFolders()
{
    static std::map<te::Edit*, juce::File> folders;
    return folders;
}

static juce::File recordingFolderFor (AssetStore& assets)
{
    // Next to this copy's audio-file folder ("Assets-recordings", "Assets-2-recordings"...).
    auto f = assets.getFolder();
    return f.getSiblingFile (f.getFileName() + "-recordings");
}

juce::File Recorder::newRecordingFile (te::Edit& e, const juce::String& extension)
{
    juce::File folder;

    {
        const juce::ScopedLock sl (folderLock());
        auto it = recordingFolders().find (&e);

        if (it != recordingFolders().end())
            folder = it->second;
    }

    if (folder == juce::File())
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SyncTest-recordings");

    folder.createDirectory();
    return folder.getNonexistentChildFile ("take", extension, false);
}

//==============================================================================
Recorder::Recorder (te::Engine& e, AssetStore& a) : engine (e), assets (a)
{
}

Recorder::~Recorder()
{
    pool.removeAllJobs (true, 30000);
    attachTo (nullptr);
}

void Recorder::attachTo (te::Edit* newEdit)
{
    if (edit != nullptr)
    {
        edit->getTransport().removeListener (this);
        const juce::ScopedLock sl (folderLock());
        recordingFolders().erase (edit);
    }

    edit = newEdit;

    if (edit == nullptr)
        return;

    edit->getTransport().addListener (this);
    const auto folder = recordingFolderFor (assets);

    {
        const juce::ScopedLock sl (folderLock());
        recordingFolders()[edit] = folder;
    }

    // Raw takes left over from earlier (they were already turned into shared files).
    if (pendingTakes == 0)
        for (auto& f : folder.findChildFiles (juce::File::findFiles, false))
            f.deleteFile();

    applyChoices();
}

//==============================================================================
juce::Array<Recorder::Input> Recorder::getInputs() const
{
    auto& dm = engine.getDeviceManager();

    // After switching interfaces the engine may pair inputs up as stereo again:
    // a mic is mono, so keep one input per socket.
    for (auto* d : dm.getWaveInputDevices())
    {
        if (d->getChannels().getNumChannels() > 1)
        {
            dm.setAllWaveInputsToNumChannels (1);
            dm.dispatchPendingUpdates();
            break;
        }
    }

    juce::Array<Input> list;

    for (auto* d : dm.getWaveInputDevices())
        list.add ({ d->getDeviceID(), d->getName() });

    return list;
}

te::InputDeviceInstance* Recorder::findInstance (const juce::String& inputID) const
{
    if (edit == nullptr || inputID.isEmpty())
        return nullptr;

    edit->getTransport().ensureContextAllocated();

    for (auto* idi : edit->getAllInputDevices())
        if (idi->getInputDevice().getDeviceID() == inputID)
            return idi;

    return nullptr;
}

void Recorder::applyChoices()
{
    if (edit == nullptr)
        return;

    for (auto& [insertID, choice] : choices)
    {
        auto* insert = song::findInsert (*edit, insertID);
        auto* idi = findInstance (choice.inputID);

        if (insert == nullptr || idi == nullptr)
            continue;

        // Heard through the insert's effects whenever it's selected (like FL).
        idi->getInputDevice().setMonitorMode (te::InputDevice::MonitorMode::on);

        if (! idi->getTargets().contains (insert->itemID))
            if (auto dest = idi->setTarget (insert->itemID, true, nullptr); ! dest)
                log ("Could not connect the input: " + dest.error());

        if (idi->isRecordingEnabled (insert->itemID) != choice.armed)
            idi->setRecordingEnabled (insert->itemID, choice.armed);
    }
}

juce::String Recorder::getInputFor (const juce::String& insertID) const
{
    auto it = choices.find (insertID);
    return it != choices.end() ? it->second.inputID : juce::String();
}

void Recorder::setInputFor (const juce::String& insertID, const juce::String& inputID)
{
    auto* insert = edit != nullptr ? song::findInsert (*edit, insertID) : nullptr;

    // Disconnect whatever fed this insert before.
    if (auto it = choices.find (insertID); it != choices.end())
    {
        if (auto* old = findInstance (it->second.inputID); old != nullptr && insert != nullptr)
            if (old->getTargets().contains (insert->itemID))
                (void) old->removeTarget (insert->itemID, nullptr);

        choices.erase (it);
    }

    if (inputID.isEmpty())
        return;

    // One input feeds one insert (selecting it here takes it away from another one).
    for (auto it = choices.begin(); it != choices.end();)
        it = it->second.inputID == inputID ? choices.erase (it) : std::next (it);

    auto& dm = engine.getDeviceManager();

    for (auto* d : dm.getWaveInputDevices())
        if (d->getDeviceID() == inputID && ! d->isEnabled())
            dm.setDeviceEnabled (*d, true);

    choices[insertID] = { inputID, true };
    applyChoices();
}

bool Recorder::isArmed (const juce::String& insertID) const
{
    auto it = choices.find (insertID);
    return it != choices.end() && it->second.armed;
}

void Recorder::setArmed (const juce::String& insertID, bool shouldBeArmed)
{
    auto it = choices.find (insertID);

    if (it == choices.end())
        return;

    it->second.armed = shouldBeArmed;
    applyChoices();
}

int Recorder::getNumArmed() const
{
    int n = 0;

    if (edit != nullptr)
        for (auto& [insertID, choice] : choices)
            if (choice.armed && song::findInsert (*edit, insertID) != nullptr && findInstance (choice.inputID) != nullptr)
                ++n;

    return n;
}

//==============================================================================
bool Recorder::isRecording() const
{
    return edit != nullptr && edit->getTransport().isRecording();
}

bool Recorder::start (double startSeconds)
{
    if (edit == nullptr || isRecording())
        return false;

    applyChoices();

    if (getNumArmed() == 0)
        return false;

    auto& transport = edit->getTransport();

    if (transport.isPlaying())
        transport.stop (false, false);

    // No count-in, no looping: the take starts right where the song start marker is
    // and simply runs on past the end of the song if you keep going.
    edit->setCountInMode (te::Edit::CountIn::none);
    transport.looping = false;
    transport.setPosition (te::TimePosition::fromSeconds (juce::jmax (0.0, startSeconds)));
    transport.record (false, false);
    return true;
}

//==============================================================================
void Recorder::recordingFinished (te::InputDeviceInstance&, te::EditItemID targetID,
                                  const juce::ReferenceCountedArray<te::Clip>& recordedClips)
{
    for (auto* clip : recordedClips)
    {
        auto* wave = dynamic_cast<te::WaveAudioClip*> (clip);

        if (wave == nullptr)
            continue;

        const auto file = wave->getSourceFileReference().getFile();
        const auto position = wave->getPosition();
        const double start = position.getStart().inSeconds();
        const double offset = position.getOffset().inSeconds();
        const double length = position.getLength().inSeconds();
        const auto insertID = targetID.toString();

        // The engine put the take on the insert itself; from now on the song model
        // plays it (as a shared file), so this engine clip goes.
        juce::MessageManager::callAsync ([c = te::Clip::Ptr (wave)] { c->removeFromParent(); });

        if (length < 0.05)
            continue;

        ++pendingTakes;
        log ("Saving the take (" + juce::String (length, 1) + " s)...");

        juce::WeakReference<Recorder> weak (this);   // made here, on the message thread

        pool.addJob ([this, weak, file, insertID, start, offset, length]
        {
            auto result = assets.importAudioFile (file);   // FLAC + fingerprint (background thread)

            juce::MessageManager::callAsync ([weak, result, file, insertID, start, offset, length]
            {
                file.deleteFile();   // the raw take; the shared copy is in the asset store now

                if (weak != nullptr)
                    weak->addTake (result, insertID, start, offset, length);
            });
        });
    }
}

void Recorder::addTake (const AssetStore::ImportResult& result, const juce::String& insertID,
                        double startSeconds, double offsetSeconds, double durationSeconds)
{
    --pendingTakes;

    if (! result.ok())
    {
        log ("Could not save the take: " + result.error);
        return;
    }

    if (edit == nullptr)
        return;

    edit->getUndoManager().beginNewTransaction();
    auto clip = song::addRecordedTake (*edit, result, insertID, startSeconds, offsetSeconds, durationSeconds);

    if (! clip.isValid())
        return;

    if (auto* channel = song::findChannel (*edit, clip[song::ids::ch].toString()))
        log ("Take added: " + channel->getName() + " (" + juce::String (durationSeconds, 1)
              + " s) - it is being sent to the others");

    if (onTakeAdded)
        onTakeAdded (clip);
}

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
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("COTOSTUDIO-recordings");

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
    stopTimer();

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

    // Which remembered choices belong to this song's inserts is decided again.
    for (auto& [id, choice] : choices)
        choice.live = false;

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

        // Another song's insert with the same ID isn't this one (a renamed insert of
        // this song still is: it was matched before it was renamed).
        if (insert == nullptr || (! choice.live && choice.insertName.isNotEmpty() && insert->getName() != choice.insertName))
        {
            choice.live = false;
            continue;
        }

        choice.live = true;
        choice.insertName = insert->getName();
        auto* idi = findInstance (choice.inputID);

        if (idi == nullptr)
            continue;

        // Heard through the insert's effects (like FL), unless monitoring is off.
        idi->getInputDevice().setMonitorMode (monitoring ? te::InputDevice::MonitorMode::on : te::InputDevice::MonitorMode::off);

        if (! idi->getTargets().contains (insert->itemID))
            if (auto dest = idi->setTarget (insert->itemID, true, nullptr); ! dest)
                log ("Could not connect the input: " + dest.error());

        if (idi->isRecordingEnabled (insert->itemID) != choice.armed)
            idi->setRecordingEnabled (insert->itemID, choice.armed);
    }
}

const Recorder::Choice* Recorder::liveChoice (const juce::String& insertID) const
{
    auto it = choices.find (insertID);
    return it != choices.end() && it->second.live ? &it->second : nullptr;
}

juce::String Recorder::getInputFor (const juce::String& insertID) const
{
    auto* c = liveChoice (insertID);
    return c != nullptr ? c->inputID : juce::String();
}

void Recorder::enableInput (const juce::String& inputID)
{
    auto& dm = engine.getDeviceManager();

    for (auto* d : dm.getWaveInputDevices())
        if (d->getDeviceID() == inputID && ! d->isEnabled())
            dm.setDeviceEnabled (*d, true);
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

    // One input feeds one insert of this song (selecting it here takes it away from
    // another one; other songs keep theirs).
    for (auto it = choices.begin(); it != choices.end();)
    {
        if (it->second.inputID == inputID && it->second.live)
        {
            if (auto* other = edit != nullptr ? song::findInsert (*edit, it->first) : nullptr)
                if (auto* old = findInstance (inputID); old != nullptr && old->getTargets().contains (other->itemID))
                    (void) old->removeTarget (other->itemID, nullptr);

            it = choices.erase (it);
        }
        else
        {
            ++it;
        }
    }

    enableInput (inputID);
    choices[insertID] = { inputID, true, insert != nullptr ? insert->getName() : juce::String(), true };
    applyChoices();
}

bool Recorder::isArmed (const juce::String& insertID) const
{
    auto* c = liveChoice (insertID);
    return c != nullptr && c->armed;
}

void Recorder::setArmed (const juce::String& insertID, bool shouldBeArmed)
{
    auto it = choices.find (insertID);

    if (it == choices.end() || ! it->second.live)
        return;

    it->second.armed = shouldBeArmed;
    applyChoices();
}

int Recorder::getNumArmed() const
{
    int n = 0;

    if (edit != nullptr)
        for (auto& [insertID, choice] : choices)
            if (choice.live && choice.armed && song::findInsert (*edit, insertID) != nullptr && findInstance (choice.inputID) != nullptr)
                ++n;

    return n;
}

juce::StringArray Recorder::getDisarmedWithInput() const
{
    juce::StringArray list;

    if (edit != nullptr)
        for (auto& [insertID, choice] : choices)
            if (choice.live && ! choice.armed && song::findInsert (*edit, insertID) != nullptr && findInstance (choice.inputID) != nullptr)
                list.add (insertID);

    return list;
}

void Recorder::disarmAll()
{
    for (auto& [insertID, choice] : choices)
        if (choice.live)
            choice.armed = false;

    applyChoices();
}

void Recorder::armAllWithInput()
{
    for (auto& [insertID, choice] : choices)
        if (choice.live)
            choice.armed = true;

    applyChoices();
}

void Recorder::setMonitoring (bool shouldMonitor)
{
    monitoring = shouldMonitor;

    if (edit == nullptr)
        return;

    for (auto& [insertID, choice] : choices)
        if (choice.live)
            if (auto* idi = findInstance (choice.inputID))
                idi->getInputDevice().setMonitorMode (monitoring ? te::InputDevice::MonitorMode::on : te::InputDevice::MonitorMode::off);
}

juce::String Recorder::saveChoices (bool onlyThisSong) const
{
    juce::StringArray lines;

    for (auto& [insertID, choice] : choices)
        if (choice.live || ! onlyThisSong)
            lines.add (insertID + "\t" + choice.insertName + "\t" + choice.inputID + "\t" + (choice.armed ? "1" : "0"));

    return lines.joinIntoString ("\n");
}

void Recorder::restoreChoices (const juce::String& text)
{
    for (auto& line : juce::StringArray::fromLines (text))
    {
        auto parts = juce::StringArray::fromTokens (line, "\t", {});

        if (parts.size() < 4 || parts[0].isEmpty() || parts[2].isEmpty())
            continue;

        // A choice made here in this run wins over a remembered one.
        if (auto it = choices.find (parts[0]); it != choices.end() && it->second.live)
            continue;

        choices[parts[0]] = { parts[2], parts[3] == "1", parts[1], false };
        enableInput (parts[2]);
    }

    applyChoices();
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

    // No looping: the take starts right where the song start marker is and simply runs
    // on past the end of the song if you keep going. With a count-in, the engine starts
    // that many bars earlier with the click, and the take still starts at the marker.
    auto countIn = countInBars == 2 ? te::Edit::CountIn::twoBar
                 : countInBars == 1 ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none;
    takePunchIn = takePunchOut = -1.0;

    if (punch.isActive())
    {
        // Punch in / out: the song plays from the pre-roll before the punch-in (you hear
        // what leads into it) and recording starts exactly at the punch-in, the same way
        // as after a count-in (lined up to the sample); recording stops at the punch-out.
        auto& ts = edit->tempoSequence;
        takePunchIn = ts.toTime (te::BeatPosition::fromBeats (punch.inBeat)).inSeconds();
        takePunchOut = ts.toTime (te::BeatPosition::fromBeats (punch.outBeat)).inSeconds();
        startSeconds = takePunchIn;
        countIn = punch.preRollBars >= 2 ? te::Edit::CountIn::twoBar
                : punch.preRollBars == 1 ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none;
    }

    edit->setCountInMode (countIn);
    transport.looping = false;
    transport.setPosition (te::TimePosition::fromSeconds (juce::jmax (0.0, startSeconds)));
    transport.record (false, false);

    if (takePunchOut >= 0.0)
    {
        // The pre-roll is just the song: no count-in clicks (the metronome, if on, still clicks).
        edit->setClickTrackRange ({});
        startTimerHz (60);   // watches for the punch-out
    }

    return true;
}

void Recorder::timerCallback()
{
    if (edit == nullptr || ! isRecording() || takePunchOut < 0.0)
    {
        stopTimer();
        return;
    }

    // Past the punch-out: recording stops, the song plays on (like FL).
    auto* ctx = edit->getTransport().getCurrentPlaybackContext();
    const double position = ctx != nullptr ? ctx->getPosition().inSeconds() : edit->getTransport().getPosition().inSeconds();

    if (position >= takePunchOut + 0.05)
    {
        stopTimer();
        edit->getTransport().stopRecording (false);
        log ("Punch out: recording stopped, the song plays on");
    }
}

double Recorder::getPreRollStart (double startSeconds) const
{
    if (edit == nullptr || countInBars == 0)
        return startSeconds;

    // Same as the engine: the count-in beats plus half a beat before the start.
    auto& ts = edit->tempoSequence;
    const double beats = ts.getTimeSig (0)->numerator * countInBars + 0.5;
    const auto start = ts.toBeats (te::TimePosition::fromSeconds (startSeconds));
    return ts.toTime (start - te::BeatDuration::fromBeats (beats)).inSeconds();
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
        double start = position.getStart().inSeconds();
        double offset = position.getOffset().inSeconds();
        double length = position.getLength().inSeconds();
        const auto insertID = targetID.toString();

        // Punch in / out: keep only the part between the punch points (the rest of the
        // recording stays in the file: drag the clip's edges out to get it back).
        if (takePunchIn >= 0.0 && takePunchOut > takePunchIn)
        {
            const double from = juce::jmax (start, takePunchIn);
            const double to = juce::jmin (start + length, takePunchOut);
            offset += from - start;
            start = from;
            length = to - from;
        }

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

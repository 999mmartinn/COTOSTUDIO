#include "Bouncer.h"
#include "PatternCompiler.h"
#include "MixerRouter.h"

Bouncer::Bouncer (te::Engine& e, AssetStore& a) : engine (e), assets (a) {}

Bouncer::~Bouncer()
{
    if (queue != nullptr)
        queue->cancelAll();

    pool.removeAllJobs (true, 30000);
    queue.reset();
    cleanUp();
}

static juce::ValueTree findClipIn (te::Edit& edit, const juce::String& sid)
{
    for (auto& c : song::getClips (edit))
        if (c[song::ids::sid].toString() == sid)
            return c;

    return {};
}

juce::String Bouncer::start (const juce::ValueTree& songState, const juce::String& clipSid, std::function<void (const Result&)> done)
{
    if (running)
        return "Still bouncing the last clip";

    result = {};
    result.clipSid = clipSid;
    planned.clear();
    onDone = std::move (done);

    // 1. A private copy of the song (its own ID, so the engine doesn't mix it up with yours).
    auto state = songState.createCopy();
    state.setProperty (te::IDs::projectID, te::ProjectItemID::createNewID (te::ProjectID {}).toString(), nullptr);
    copy = te::loadEditFromState (engine, state, te::Edit::forRendering);

    if (copy == nullptr)
        return "Couldn't make a copy of the song to bounce";

    auto clip = findClipIn (*copy, clipSid);

    if (! clip.isValid() || song::isAutomationClip (clip))
    {
        cleanUp();
        return "That clip doesn't exist any more";
    }

    const double startBeat = clip[song::ids::start];
    const double length = song::getClipLength (*copy, clip);
    result.startBeat = startBeat;

    // 2. Which channels it plays, and what the new audio will be called.
    struct Source { te::AudioTrack* track; juce::String name; };
    std::vector<Source> sources;

    if (song::isAudioClip (clip))
    {
        if (auto* t = song::findChannel (*copy, clip[song::ids::ch].toString()))
            sources.push_back ({ t, song::getAssetName (*copy, clip[song::ids::asset].toString())
                                      .upToLastOccurrenceOf (".", false, false) + " (bounce)" });
    }
    else
    {
        auto pattern = song::findPattern (*copy, clip[song::ids::pattern].toString());
        const double from = song::getPatternOffset (clip);

        for (auto* t : song::getChannels (*copy))
        {
            bool plays = false;

            for (auto& n : song::getNotes (pattern, song::getChannelID (*t)))
                if ((double) n[song::ids::b] >= from - 1.0e-6 && (double) n[song::ids::b] < from + length - 1.0e-6)
                    plays = true;

            if (plays)
                sources.push_back ({ t, pattern[song::ids::name].toString() + " - " + t->getName() });
        }
    }

    if (sources.empty())
    {
        cleanUp();
        return "Nothing plays in that clip";
    }

    // 3. Only this clip plays (automation clips stay: they shape the sound).
    for (auto& c : song::getClips (*copy))
        if (c != clip && ! song::isAutomationClip (c))
            song::removeClip (*copy, c);

    // Each channel plays straight out in the copy: rendered on its own, a channel routed into
    // an insert would be silent (its sound goes on into the insert, which isn't rendered).
    // The new audio goes through that same insert afterwards (see apply).
    for (auto& src : sources)
        song::setRoute (*copy, *src.track, {});

    compiler = std::make_unique<PatternCompiler> (*copy, assets);
    compiler->setSongMode (true);
    compiler->compileNow();
    router = std::make_unique<MixerRouter> (*copy);
    router->applyNow();

    const double startSeconds = song::beatsToSeconds (*copy, startBeat);
    clipSeconds = song::beatsToSeconds (*copy, startBeat + length) - startSeconds;
    constexpr double tailSeconds = 8.0;   // room for releases and reverbs (trimmed afterwards)

    // 4. One render per channel: as it sounds before the mixer (no inserts, no master).
    te::RenderSpecification spec;
    spec.time = te::TimeRange (te::TimePosition::fromSeconds (startSeconds),
                               te::TimePosition::fromSeconds (startSeconds + clipSeconds + tailSeconds));
    spec.sampleRate = 44100.0;
    spec.channelLayout = "stereo";
    spec.format = te::RenderFormat::wav;
    spec.bitDepth = 24;
    spec.includeTails = false;
    spec.useMasterPlugins = false;

    queue = std::make_unique<te::RenderQueue>();
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("COTOSTUDIO bounce");
    folder.createDirectory();

    for (auto& s : sources)
    {
        auto job = spec;
        job.tracks = { s.track->itemID };
        job.destination = folder.getNonexistentChildFile ("bounce", ".wav", false);

        if (te::validateRenderSpecification (*copy, job).failed())
            continue;

        auto planned_ = te::createRenderJob (*copy, job);

        if (! planned_.has_value())
            continue;

        planned_->params.checkNodesForAudio = false;   // a silent channel still gives a (silent) file
        planned_->name = s.name;
        queue->addJob (std::move (*planned_));
        planned.push_back ({ song::getChannelID (*s.track), s.name, job.destination });
    }

    if (planned.empty())
    {
        cleanUp();
        return "Couldn't set up the bounce";
    }

    queue->onFinished = [this] { rendersFinished(); };
    running = true;
    queue->start();
    return {};
}

void Bouncer::rendersFinished()
{
    if (! running)
        return;

    // Trimming and adding to the store reads and writes whole files: in the background.
    pool.addJob ([this, weak = juce::WeakReference<Bouncer> (this), jobs = planned, minSeconds = clipSeconds]
    {
        std::vector<Part> parts;
        int silent = 0;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        for (auto& p : jobs)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (p.file));

            if (reader == nullptr || reader->lengthInSamples <= 0)
            {
                ++silent;
                p.file.deleteFile();
                continue;
            }

            const int numChannels = (int) juce::jmax (1u, reader->numChannels);
            const int length = (int) reader->lengthInSamples;
            juce::AudioBuffer<float> audio (numChannels, length);
            reader->read (&audio, 0, length, 0, true, true);
            const double rate = reader->sampleRate;
            reader = nullptr;
            p.file.deleteFile();

            // Where the sound ends (-60 dB): the clip lasts at least as long as the original.
            int last = -1;

            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = length; --i > last;)
                    if (std::abs (audio.getSample (ch, i)) > 0.001f)
                    {
                        last = i;
                        break;
                    }

            if (last < 0)
            {
                ++silent;
                continue;
            }

            const int keep = juce::jlimit (1, length, juce::jmax ((int) std::ceil (minSeconds * rate), last + (int) (0.05 * rate)));
            const auto trimmed = p.file.getSiblingFile (p.file.getFileNameWithoutExtension() + " trimmed.wav");

            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> out (trimmed.createOutputStream());

                if (out == nullptr)
                    continue;

                auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (rate)
                                                          .withNumChannels (numChannels).withBitsPerSample (24));

                if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, keep))
                {
                    trimmed.deleteFile();
                    continue;
                }
            }

            auto imported = weak != nullptr ? assets.importAudioFile (trimmed) : AssetStore::ImportResult();
            trimmed.deleteFile();

            if (imported.ok())
                parts.push_back ({ p.channelID, p.channelName, imported, keep / rate });
        }

        juce::MessageManager::callAsync ([weak, parts, silent]
        {
            if (weak == nullptr)
                return;

            weak->result.parts = parts;
            weak->result.silentChannels = silent;

            if (parts.empty())
                weak->result.error = silent > 0 ? "The clip renders silent on this PC (a plugin missing here?)" : "The bounce failed";

            weak->finish();
        });
    });
}

void Bouncer::finish()
{
    running = false;

    for (auto& job : queue != nullptr ? queue->getJobs() : std::vector<te::RenderQueue::JobPtr>())
        if (job->getState() == te::RenderQueue::Job::State::failed && result.parts.empty() && result.error.isEmpty())
            result.error = "The bounce failed: " + job->getError();

    // The render queue and the copy go once we're out of its callback.
    juce::MessageManager::callAsync ([weak = juce::WeakReference<Bouncer> (this), r = result, cb = onDone]
    {
        if (weak != nullptr)
        {
            weak->queue.reset();
            weak->cleanUp();
        }

        if (cb)
            cb (r);
    });
}

void Bouncer::cleanUp()
{
    for (auto& p : planned)
        p.file.deleteFile();

    router.reset();
    compiler.reset();
    copy.reset();
}

//==============================================================================
juce::Array<juce::ValueTree> Bouncer::apply (te::Edit& edit, const Result& r)
{
    juce::Array<juce::ValueTree> added;

    if (! r.ok())
        return added;

    // The original clip goes (if nobody deleted it meanwhile); its track takes the first part.
    auto original = findClipIn (edit, r.clipSid);

    if (! original.isValid())
        return added;   // deleted meanwhile (or another song): nothing to replace

    const int firstLane = original[song::ids::lane];
    song::removeClip (edit, original);

    auto laneIsFree = [&edit] (int lane, double start, double end)
    {
        for (auto& c : song::getClips (edit))
            if ((int) c[song::ids::lane] == lane && (double) c[song::ids::start] < end
                 && (double) c[song::ids::start] + song::getClipLength (edit, c) > start)
                return false;

        return true;
    };

    int nextLane = firstLane;

    for (size_t i = 0; i < r.parts.size(); ++i)
    {
        auto& part = r.parts[i];
        const double endBeat = song::secondsToBeats (edit, song::beatsToSeconds (edit, r.startBeat) + part.seconds);
        int lane = i == 0 ? firstLane : -1;

        for (int l = nextLane; lane < 0 && l < song::numLanes; ++l)
            if (laneIsFree (l, r.startBeat, endBeat))
                lane = l;

        if (lane < 0)
            lane = juce::jmin (song::numLanes - 1, firstLane + (int) i);

        nextLane = lane + 1;
        auto clip = song::addImportedAudio (edit, part.audio, part.channelName + ".wav", lane, r.startBeat);

        if (! clip.isValid())
            continue;

        clip.setProperty (song::ids::duration, juce::jmax (0.01, part.seconds), &edit.getUndoManager());

        // Through the same insert as the channel it came from.
        if (auto* source = song::findChannel (edit, part.channelID))
            if (auto* channel = song::findChannel (edit, clip[song::ids::ch].toString()))
                song::setRoute (edit, *channel, song::getRoute (*source));

        added.add (clip);
    }

    return added;
}

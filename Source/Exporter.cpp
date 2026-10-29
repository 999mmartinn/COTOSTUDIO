#include "Exporter.h"
#include "PatternCompiler.h"
#include "MixerRouter.h"
#include "Mp3Encoder.h"

Exporter::Exporter (te::Engine& e, AssetStore& a) : engine (e), assets (a) {}

Exporter::~Exporter()
{
    cancelEncoding = true;
    encoderPool.removeAllJobs (true, 20000);
    stopTimer();
    queue.reset();
    cleanUp();
}

bool Exporter::canExportMp3()                   { return mp3::isAvailable(); }

juce::String Exporter::extensionFor (Format f)
{
    switch (f)
    {
        case Format::flac:                          return ".flac";
        case Format::mp3_320: case Format::mp3_192: return ".mp3";
        case Format::wav24:   case Format::wav16:   break;
    }

    return ".wav";
}

juce::String Exporter::describe (Format f)
{
    switch (f)
    {
        case Format::wav24:   return "WAV, 24-bit (best for mixing/mastering)";
        case Format::wav16:   return "WAV, 16-bit (CD quality)";
        case Format::flac:    return "FLAC (lossless, smaller)";
        case Format::mp3_320: return "MP3, 320 kbps (to share)";
        case Format::mp3_192: return "MP3, 192 kbps (smaller)";
    }

    return {};
}

static juce::String safeFileName (const juce::String& s)
{
    auto name = juce::File::createLegalFileName (s.trim());
    return name.isNotEmpty() ? name : juce::String ("untitled");
}

//==============================================================================
juce::String Exporter::start (const juce::ValueTree& songState, const Options& opts, std::function<void (const Result&)> done)
{
    if (running)
        return "An export is already running";

    const bool mp3 = opts.format == Format::mp3_320 || opts.format == Format::mp3_192;

    if (mp3 && ! canExportMp3())
        return "MP3 export needs Windows' MP3 encoder (Windows 8 or newer)";

    options = opts;
    onFinished = std::move (done);
    result = {};
    planned.clear();
    progress = 0.0;
    encodedSoFar = 0;
    cancelEncoding = false;

    // 1. A private copy of the song, with its own ID so the engine doesn't mix it up
    //    with the song you're working on.
    auto state = songState.createCopy();
    state.setProperty (te::IDs::projectID, te::ProjectItemID::createNewID (te::ProjectID {}).toString(), nullptr);
    copy = te::loadEditFromState (engine, state, te::Edit::forRendering);

    if (copy == nullptr)
        return "Couldn't make a copy of the song to export";

    for (auto& hash : song::getAssetHashes (*copy))
        if (! assets.has (hash))
            ++result.missingAudio;

    for (auto* plugin : te::getAllPlugins (*copy, false))
        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (plugin))
            if (ext->getAudioPluginInstance() == nullptr)
                ++result.missingPlugins;

    // 2. What plays: the arrangement, or one pattern; the mixer as everyone has it (no solo).
    compiler = std::make_unique<PatternCompiler> (*copy, assets);
    compiler->setSongMode (opts.wholeSong);

    if (! opts.wholeSong)
        compiler->setPattern (opts.patternSid);

    compiler->compileNow();
    router = std::make_unique<MixerRouter> (*copy);
    router->applyNow();

    double lengthBeats = 0.0;

    if (opts.wholeSong)
    {
        if (song::getClips (*copy).isEmpty())
        {
            cleanUp();
            return "The playlist is empty: place some patterns or audio in it, or export the current pattern";
        }

        lengthBeats = song::getSongLengthBeats (*copy);
    }
    else
    {
        auto pattern = song::findPattern (*copy, opts.patternSid);

        if (! pattern.isValid())
        {
            cleanUp();
            return "That pattern doesn't exist any more";
        }

        lengthBeats = song::getLengthBeats (pattern);
    }

    const te::TimeRange range (te::TimePosition(), copy->tempoSequence.toTime (te::BeatPosition::fromBeats (lengthBeats)));

    // 3. The jobs: the mix, then (optionally) one file per insert.
    te::RenderSpecification base;
    base.time = range;
    base.sampleRate = 44100.0;
    base.channelLayout = "stereo";
    base.format = opts.format == Format::flac ? te::RenderFormat::flac : te::RenderFormat::wav;
    base.bitDepth = opts.format == Format::wav24 || opts.format == Format::flac ? 24 : 16;
    base.includeTails = opts.wholeSong;      // let reverbs ring out at the end of the song...
    base.wrapRemainder = ! opts.wholeSong;   // ...a pattern becomes a seamless loop instead

    auto addJob = [this, mp3] (te::RenderSpecification spec, const juce::File& finalFile, bool isStem) -> bool
    {
        auto renderFile = mp3 ? finalFile.getSiblingFile (finalFile.getFileNameWithoutExtension() + ".rendering.wav")
                              : finalFile.getSiblingFile (finalFile.getFileNameWithoutExtension() + ".rendering"
                                                          + finalFile.getFileExtension());
        spec.destination = renderFile;

        if (te::validateRenderSpecification (*copy, spec).failed())
            return false;

        auto job = te::createRenderJob (*copy, spec);

        if (! job.has_value())
            return false;

        if (isStem)
            job->params.checkNodesForAudio = false;   // a silent insert still gets its (silent) file

        job->name = finalFile.getFileName();
        queue->addJob (std::move (*job));
        planned.push_back ({ renderFile, finalFile });
        return true;
    };

    queue = std::make_unique<te::RenderQueue>();
    auto destination = opts.destination.withFileExtension (extensionFor (opts.format));
    destination.getParentDirectory().createDirectory();

    if (! addJob (base, destination, false))
    {
        cleanUp();
        return "Couldn't set up the export (is the song silent?)";
    }

    if (opts.stems)
    {
        auto folder = destination.getSiblingFile (destination.getFileNameWithoutExtension() + " - stems");
        folder.createDirectory();

        auto stem = base;
        stem.useMasterPlugins = false;     // each insert as it sounds before the master
        stem.includeSourceTracks = true;   // sends into it keep playing (but aren't heard themselves)

        // Channels that play straight into the master.
        juce::Array<te::EditItemID> direct;

        for (auto* t : song::getChannels (*copy))
            if (song::resolveRoute (*copy, *t) == nullptr)
                direct.add (t->itemID);

        if (! direct.isEmpty())
        {
            stem.tracks = direct;
            addJob (stem, folder.getChildFile ("00 Direct to master" + extensionFor (opts.format)), true);
        }

        for (auto* insert : song::getInserts (*copy))
        {
            stem.tracks = { insert->itemID };
            const auto name = juce::String (song::getInsertNumber (*copy, *insert)).paddedLeft ('0', 2) + " " + insert->getName();
            addJob (stem, folder.getChildFile (safeFileName (name) + extensionFor (opts.format)), true);
        }
    }

    const int numJobs = (int) planned.size();

    queue->onJobStarted = [this] (te::RenderQueue::Job& job)
    {
        status = "Rendering " + job.getName() + "...";
    };

    queue->onFinished = [this] { rendersFinished(); };

    running = true;
    status = "Rendering...";
    queue->start();
    startTimerHz (10);
    juce::ignoreUnused (numJobs);
    return {};
}

void Exporter::timerCallback()
{
    if (queue == nullptr)
        return;

    const auto& jobs = queue->getJobs();
    const bool mp3 = options.format == Format::mp3_320 || options.format == Format::mp3_192;
    const double renderShare = mp3 ? 0.8 : 1.0;
    double done = 0.0;

    for (auto& job : jobs)
        done += job->getState() == te::RenderQueue::Job::State::pending ? 0.0
              : job->getState() == te::RenderQueue::Job::State::active ? (double) job->getProgress() : 1.0;

    const double renders = jobs.empty() ? 0.0 : done / (double) jobs.size();
    const double encodes = planned.empty() ? 0.0 : (double) encodedSoFar.load() / (double) planned.size();
    progress = juce::jlimit (0.0, 1.0, renders * renderShare + encodes * (1.0 - renderShare));
}

void Exporter::cancel()
{
    if (! running)
        return;

    result.cancelled = true;
    cancelEncoding = true;

    if (queue != nullptr)
        queue->cancelAll();   // onFinished follows
}

void Exporter::rendersFinished()
{
    if (! running)
        return;

    // Jobs that didn't make it.
    for (size_t i = 0; i < queue->getJobs().size(); ++i)
    {
        auto& job = queue->getJobs()[i];

        if (job->getState() == te::RenderQueue::Job::State::cancelled)
            result.cancelled = true;
        else if (job->getState() == te::RenderQueue::Job::State::failed && result.error.isEmpty() && i == 0)
            result.error = "The export failed: " + job->getError();
    }

    if (result.cancelled || result.error.isNotEmpty())
    {
        finish();
        return;
    }

    const bool mp3 = options.format == Format::mp3_320 || options.format == Format::mp3_192;

    if (mp3)
    {
        encodeMp3s();
        return;
    }

    for (auto& p : planned)
    {
        if (! p.renderFile.existsAsFile())
            continue;

        p.finalFile.deleteFile();

        if (p.renderFile.moveFileTo (p.finalFile))
            result.files.add (p.finalFile);
    }

    finish();
}

void Exporter::encodeMp3s()
{
    status = "Encoding MP3...";
    const int kbps = options.format == Format::mp3_320 ? 320 : 192;
    auto jobs = planned;

    encoderPool.addJob ([this, weak = juce::WeakReference<Exporter> (this), jobs, kbps]
    {
        juce::String error;
        juce::Array<juce::File> done;

        for (auto& p : jobs)
        {
            if (cancelEncoding)
                break;

            if (! p.renderFile.existsAsFile())
                continue;

            auto e = mp3::encodeWavFile (p.renderFile, p.finalFile, kbps, cancelEncoding);
            p.renderFile.deleteFile();

            if (e.isNotEmpty())
            {
                if (error.isEmpty())
                    error = e;

                p.finalFile.deleteFile();
            }
            else
            {
                done.add (p.finalFile);
            }

            ++encodedSoFar;
        }

        juce::MessageManager::callAsync ([weak, error, done]
        {
            if (weak == nullptr)
                return;

            auto& self = *weak;

            if (self.cancelEncoding)
                self.result.cancelled = true;
            else if (error.isNotEmpty())
                self.result.error = error;

            self.result.files = done;
            self.finish();
        });
    });
}

void Exporter::finish()
{
    stopTimer();
    running = false;
    progress = 1.0;

    if (! result.ok())
        for (auto& f : result.files)
            f.deleteFile();   // a cancelled or failed export leaves nothing half-done behind

    if (! result.ok())
        result.files.clear();

    // The render queue and the copy go once we're out of its callback.
    juce::MessageManager::callAsync ([weak = juce::WeakReference<Exporter> (this), r = result, cb = onFinished]
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

void Exporter::cleanUp()
{
    for (auto& p : planned)
        p.renderFile.deleteFile();

    router.reset();
    compiler.reset();
    copy.reset();
}

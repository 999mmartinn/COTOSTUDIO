// Automatic test: one host and two joiners in the same process, talking over
// real TCP sockets on 127.0.0.1. Run with:
//   SyncTest --selftest [--plugin=<path to a VST3 effect>] [--dump=<dir>]

#include "SelfTest.h"
#include <iostream>
#include <thread>
#include <mutex>
#include "Session.h"
#include "SongModel.h"
#include "PatternCompiler.h"
#include "DrumSynthPlugin.h"
#include "MixerRouter.h"
#include "ProjectFile.h"
#include "Exporter.h"
#include "SamplerPlugin.h"
#include "SamplePreviewer.h"
#include "FileImport.h"
#include "AppData.h"

namespace
{
    struct Step
    {
        juce::String name;
        std::function<void()> action;
        std::function<bool()> done;
        int timeoutMs = 8000;
        int minWaitMs = 0;
        bool optional = false;
    };
}

namespace
{
    /** A sound card simulated through the engine's "hosted" audio device: one mono
        input (the "microphone") that the test feeds by hand, and a stereo output. */
    struct SimulatedSoundCard
    {
        static constexpr double sampleRate = 44100.0;
        static constexpr int blockSize = 512;

        explicit SimulatedSoundCard (te::Engine& e)
            : engine (e), io (e.getDeviceManager().getHostedAudioDeviceInterface())
        {
            // Blocks are fed faster than real time: don't let the CPU guard mute them.
            engine.getDeviceManager().setCpuLimitBeforeMuting (1000.0);

            te::HostedAudioDeviceInterface::Parameters p;
            p.sampleRate = sampleRate;
            p.blockSize = blockSize;
            p.inputChannels = 1;
            p.outputChannels = 2;
            io.initialise (p);
            io.prepareToPlay (sampleRate, blockSize);
            engine.getDeviceManager().dispatchPendingUpdates();
        }

        ~SimulatedSoundCard()
        {
            stopFeeding();
            engine.getDeviceManager().deviceManager.closeAudioDevice();
            engine.getDeviceManager().removeHostedAudioDeviceInterface();
        }

        /** Sings into the "mic": a sine of this level for this long (level 0 = silence).
            Always whole blocks, like a real sound card (the hosted device holds back
            partial blocks, which would blur the timing checks). 437.31 Hz so the wave's
            phase never repeats within the test and identifies each input sample. */
        void feed (double seconds, float level, double frequency = 437.31)
        {
            const int total = juce::jmax (1, (int) std::ceil (seconds * sampleRate / blockSize)) * blockSize;
            juce::MidiBuffer midi;

            for (int done = 0; done < total;)
            {
                const int n = juce::jmin (blockSize, total - done);
                juce::AudioBuffer<float> buffer (2, n);
                buffer.clear();

                for (int i = 0; i < n; ++i)
                    buffer.setSample (0, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * frequency
                                                                       * (double) (samplesFed + i) / sampleRate));

                io.processBlock (buffer, midi);
                samplesFed += n;
                done += n;
            }
        }

        /** Feeds silence into the mic and returns the loudest sample that came out
            of the "speakers" (what the engine and anything else on the card played). */
        float feedSilenceAndMeasure (double seconds)
        {
            const int total = juce::jmax (1, (int) std::ceil (seconds * sampleRate / blockSize)) * blockSize;
            juce::MidiBuffer midi;
            float peak = 0.0f;

            for (int done = 0; done < total; done += blockSize)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                io.processBlock (buffer, midi);
                peak = juce::jmax (peak, buffer.getMagnitude (0, blockSize));
                samplesFed += blockSize;
            }

            return peak;
        }

        /** Like a real interface: keeps processing blocks of tone on its own thread
            (a little faster than real time) until stopFeeding(). After each block it notes
            where this Edit's playhead was, so the test can tell which input sample was
            going in at any song position. */
        void startFeeding (float level, te::Edit& editToWatch)
        {
            stopFeeding();
            keepFeeding = true;
            watched = &editToWatch;

            feeder = std::thread ([this, level]
            {
                while (keepFeeding)
                {
                    feed ((double) blockSize / sampleRate, level);

                    if (auto* ctx = watched->getTransport().getCurrentPlaybackContext())
                    {
                        const std::lock_guard<std::mutex> lock (marksLock);
                        marks.push_back ({ samplesFed, ctx->getPosition().inSeconds() });
                    }

                    std::this_thread::sleep_for (std::chrono::milliseconds (5));
                }
            });
        }

        void stopFeeding()
        {
            keepFeeding = false;

            if (feeder.joinable())
                feeder.join();
        }

        /** The input sample going in when the playhead was at this time (from a block
            played after that time, while the song ran without jumps). -1 if unknown. */
        juce::int64 inputSampleAt (double seconds)
        {
            const std::lock_guard<std::mutex> lock (marksLock);

            for (auto& m : marks)
                if (m.second > seconds + 0.05)
                    return m.first - (juce::int64) std::llround ((m.second - seconds) * sampleRate);

            return -1;
        }

        double lastPosition()
        {
            const std::lock_guard<std::mutex> lock (marksLock);
            return marks.empty() ? -1.0 : marks.back().second;
        }

        te::Engine& engine;
        te::HostedAudioDeviceInterface& io;
        std::atomic<juce::int64> samplesFed { 0 };
        std::thread feeder;
        std::atomic<bool> keepFeeding { false };
        te::Edit* watched = nullptr;
        std::mutex marksLock;
        std::vector<std::pair<juce::int64, double>> marks;
    };
}

class SelfTest::Impl  : private juce::Timer
{
public:
    Impl (te::Engine& e, const juce::StringArray& args, std::function<void (int)> onFinished)
        : engine (e), finished (std::move (onFinished))
    {
        for (auto& a : args)
        {
            if (a.startsWith ("--plugin="))   pluginFile = juce::File (a.fromFirstOccurrenceOf ("=", false, false).unquoted());
            if (a.startsWith ("--dump="))     dumpDir = juce::File (a.fromFirstOccurrenceOf ("=", false, false).unquoted());
        }

        // Each simulated PC gets its own folder of audio files, like separate computers.
        testRoot = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("synctest-selftest");
        testRoot.deleteRecursively();   // leftovers from an earlier run (ignored if locked)
        testRoot.createDirectory();

        for (auto* s : { &host, &peer1, &peer2 })
        {
            auto* label = s == &host ? "host" : (s == &peer1 ? "peer1" : "peer2");
            s->peerName = label;
            s->onLog = [label] (const juce::String& m) { std::cout << "    [" << label << "] " << m << std::endl; };
            s->setAssetFolder (testRoot.getChildFile (juce::String (label) + "-assets"));
            s->onTempoQuestion = [this, s] (const collab::Session::TempoQuestion& q) { tempoAsked[s].add (q); };
            s->onTempoQuestionClosed = [this, s] (int id) { tempoClosed[s].add (id); };
            s->onSharedPatternOffered = [this, s] (const juce::String& sid, const juce::String&) { patternOffers[s].add (sid); };
        }

        toneFile  = writeTestWav (testRoot.getChildFile ("tone.wav"), 2.0, 440.0, false);
        voiceFile = writeTestWav (testRoot.getChildFile ("voice.wav"), 1.0, 220.0, false);
        bigFile   = writeTestWav (testRoot.getChildFile ("big.wav"), 60.0, 0.0, true);   // ~10 MB as FLAC

        buildSteps();
        startTimer (20);
    }

private:
    te::Engine& engine;
    std::function<void (int)> finished;
    juce::File pluginFile, dumpDir;
    std::unique_ptr<SimulatedSoundCard> soundCard;   // declared first: outlives the sessions
    std::unique_ptr<SamplePreviewer> previewer;       // the sound browser's preview (on the simulated card)
    collab::Session host { engine }, peer1 { engine }, peer2 { engine };
    collab::Session fourth { engine };   // a PC that only opens a saved song
    std::vector<Step> steps;
    size_t current = 0;
    bool actionRun = false;
    juce::uint32 stepStart = 0;
    int passed = 0, failed = 0, skipped = 0;
    juce::StringArray failures;
    juce::String deletedChannelID;
    bool loopRefused = false;

    // Audio file sharing
    juce::File testRoot, toneFile, voiceFile, bigFile;
    juce::String toneHash, voiceHash, bigHash;
    bool bigStillTransferring = false;

    // People, chat, browser
    juce::StringArray compiledBeforeSplit;

    // Song files
    juce::File projectFile, legacyFile;
    appdata::MigrationResult migrationResult;
    juce::String migrationHash;
    project::SaveResult saveResult;
    bool saveDone = false, openDone = false;
    juce::String openError { "not done" }, newError { "not done" };
    int resyncsBefore = 0, channelsBefore = 0;

    // Tempo approval, sharing playback
    std::map<collab::Session*, juce::Array<collab::Session::TempoQuestion>> tempoAsked;
    std::map<collab::Session*, juce::Array<int>> tempoClosed;
    std::map<collab::Session*, juce::StringArray> patternOffers;
    juce::String shareError { "not done" };
    double shareOffset = 0.0;

    // Solo, sends, export, count-in
    juce::String sendsCheck;
    float sendOpen = 0.0f, sendClosed = 0.0f;
    std::unique_ptr<Exporter> exporter;
    bool exportDone = false;
    Exporter::Result exportResult;
    juce::int64 fedAtTake2Start = -1;

    // Your sounds (sound browser): preview, sampler channels, MIDI files
    juce::File beatsFolder, boomFile, longFile, midiFile;
    juce::String boomHash, longHash, midiLogOnSynth, midiLogNew;
    juce::int64 contentBeforePreview = 0;
    int assetsBeforePreview = 0;
    bool p2WasSongMode = true, exportCopyLoaded = false;

    /** A mono drum-like hit: a sine that fades out over the file. */
    static juce::File writeHitWav (juce::File f, double seconds, double freq)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream());
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));

        if (writer == nullptr)
            return f;

        const int n = (int) (seconds * 44100.0);
        juce::AudioBuffer<float> buffer (1, n);

        for (int i = 0; i < n; ++i)
        {
            const double t = i / 44100.0;
            buffer.setSample (0, i, (float) (0.6 * std::exp (-t / (seconds * 0.25)) * std::sin (juce::MathConstants<double>::twoPi * freq * t)));
        }

        writer->writeFromAudioSampleBuffer (buffer, 0, n);
        return f;
    }

    /** A MIDI file with 4 notes in the first 2 bars, and one far away at bar 11. */
    static juce::File writeTestMidi (juce::File f)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        juce::MidiMessageSequence seq;
        struct N { int pitch; double beat, length; int velocity; };

        for (auto n : { N { 60, 0.0, 1.0, 100 }, N { 64, 1.0, 1.0, 90 }, N { 67, 2.0, 0.5, 80 }, N { 72, 4.0, 2.0, 110 },
                        N { 48, 40.0, 1.0, 100 } })
        {
            seq.addEvent (juce::MidiMessage::noteOn (1, n.pitch, (juce::uint8) n.velocity), n.beat * 960.0);
            seq.addEvent (juce::MidiMessage::noteOff (1, n.pitch), (n.beat + n.length) * 960.0);
        }

        seq.updateMatchedPairs();
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (960);
        midi.addTrack (seq);
        juce::FileOutputStream out (f);
        midi.writeTo (out);
        return f;
    }

    /** A channel's notes in Pattern 1 as sorted "pitch@beat+length v velocity" strings. */
    static juce::StringArray notesIn (te::Edit& e, const juce::String& channelName)
    {
        juce::StringArray list;

        if (auto* t = ch (e, channelName))
            for (auto& n : song::getNotes (pat (e, "Pattern 1"), song::getChannelID (*t)))
                list.add (juce::String ((int) n[song::ids::p]) + "@" + juce::String ((double) n[song::ids::b], 2)
                           + "+" + juce::String ((double) n[song::ids::l], 2) + "v" + juce::String ((int) n[song::ids::v]));

        list.sort (true);
        return list;
    }

    /** Renders some channels of an Edit (its loop range) and measures the peak in several beat windows. */
    juce::Array<float> renderPeaks (te::Edit& e, juce::StringArray trackNames, std::initializer_list<std::pair<double, double>> windows)
    {
        juce::Array<float> peaks;
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("synctest-render", ".wav");
        juce::BigInteger tracks;
        auto all = te::getAllTracks (e);

        for (int i = 0; i < all.size(); ++i)
            if (trackNames.contains (all[i]->getName()))
                tracks.setBit (i);

        const bool ok = te::Renderer::renderToFile ("test", file, e, e.getTransport().getLoopRange(), tracks, true, true, {}, false);

        for (auto& w : windows)
            peaks.add (ok ? peakOf (file, timeOfBeat (e, w.first), timeOfBeat (e, w.second)) : -1.0f);

        file.deleteFile();
        return peaks;
    }

    // Automation
    juce::String autoSid;
    int sentBeforeAutomation = 0;
    float autoEarly = -1.0f, autoLate = -1.0f, autoQuiet = -1.0f, autoLoud = -1.0f;

    juce::ValueTree autoClip (te::Edit& e)
    {
        for (auto& c : song::getClips (e))
            if (c[song::ids::sid].toString() == autoSid)
                return c;

        return {};
    }

    static te::AutomatableParameter* kickVolume (te::Edit& e)
    {
        return song::getVolumePlugin (*ch (e, "Kick"))->volParam.get();
    }

    float renderPeakBetween (te::Edit& e, juce::StringArray trackNames, double fromBeat, double toBeat)
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("synctest-render", ".wav");
        juce::BigInteger tracks;
        auto all = te::getAllTracks (e);

        for (int i = 0; i < all.size(); ++i)
            if (trackNames.contains (all[i]->getName()))
                tracks.setBit (i);

        if (! te::Renderer::renderToFile ("test", file, e, e.getTransport().getLoopRange(), tracks, true, true, {}, false))
            return -1.0f;

        const float peak = peakOf (file, timeOfBeat (e, fromBeat), timeOfBeat (e, toBeat));
        file.deleteFile();
        return peak;
    }
    double preRollSeconds = 0.0;

    static juce::String outputOf (te::Edit& e, te::AudioTrack* t)
    {
        return t != nullptr ? t->getOutput().getOutputName() : juce::String ("?");
    }

    static int countGenerated (te::AudioTrack* t, bool sends)
    {
        int n = 0;

        if (t != nullptr)
            for (auto* p : t->pluginList.getPlugins())
                if (song::isGeneratedPlugin (p) && (sends ? dynamic_cast<te::AuxSendPlugin*> (p) != nullptr
                                                          : dynamic_cast<te::AuxReturnPlugin*> (p) != nullptr))
                    ++n;

        return n;
    }

    void startExport (const juce::File& dest, bool song, Exporter::Format format, bool stems)
    {
        exportDone = false;
        exportResult = {};
        exporter = std::make_unique<Exporter> (engine, host.getAssetStore());

        Exporter::Options o;
        o.wholeSong = song;
        o.patternSid = pat (H(), "Pattern 1")[song::ids::sid].toString();
        o.format = format;
        o.stems = stems;
        o.destination = dest;
        dest.getParentDirectory().deleteRecursively();

        auto error = exporter->start (host.getSync()->createSnapshot(), o, [this] (const Exporter::Result& r)
        {
            exportResult = r;
            exportDone = true;
        });

        if (error.isNotEmpty())
        {
            exportResult.error = error;
            exportDone = true;
        }
    }

    static double lengthOf (const juce::File& f)
    {
        double length = -1.0;
        peakOf (f, 0.0, 0.01, &length);
        return length;
    }
    juce::int64 newSongHash = 0;

    int lastQuestion (collab::Session& s) { auto& a = tempoAsked[&s]; return a.isEmpty() ? 0 : a.getLast().id; }
    bool questionOpen (collab::Session& s)
    {
        const int id = lastQuestion (s);
        return id != 0 && ! tempoClosed[&s].contains (id);
    }

    static double bpmOf (te::Edit& e)       { return e.tempoSequence.getTempo (0)->getBpm(); }
    static double beatOf (te::Edit& e)      { return e.tempoSequence.toBeats (e.getTransport().getPosition()).inBeats(); }
    static double timeOfBeat (te::Edit& e, double beat) { return e.tempoSequence.toTime (te::BeatPosition::fromBeats (beat)).inSeconds(); }

    bool compiledEqual (collab::Session& a, collab::Session& b)
    {
        for (auto* t : song::getChannels (*a.getEdit()))
            if (compiled (a, t->getName()) != compiled (b, t->getName())
                 || compiledAudio (a, t->getName()) != compiledAudio (b, t->getName()))
                return false;

        return true;
    }

    // Recording
    juce::String vocalsID, takeHash;
    bool recordStarted = false;
    juce::int64 fedAtTakeStart = -1;   // which input sample was playing at the take's start (measured)
    static constexpr int port = 47811;

    te::Edit& H()  { return *host.getEdit(); }
    te::Edit& P1() { return *peer1.getEdit(); }
    te::Edit& P2() { return *peer2.getEdit(); }

    //==============================================================================
    static te::AudioTrack* ch (te::Edit& e, const juce::String& name)
    {
        for (auto* t : song::getChannels (e))
            if (t->getName() == name)
                return t;

        return nullptr;
    }

    static juce::ValueTree pat (te::Edit& e, const juce::String& name)
    {
        for (auto& p : song::getPatterns (e))
            if (p[song::ids::name].toString() == name)
                return p;

        return {};
    }

    static bool hasStep (te::Edit& e, const juce::String& patternName, const juce::String& channelName, int step)
    {
        auto p = pat (e, patternName);
        auto* t = ch (e, channelName);
        return p.isValid() && t != nullptr && song::hasStep (p, song::getChannelID (*t), step);
    }

    static juce::ValueTree note (te::Edit& e, const juce::String& patternName, const juce::String& channelName, int pitch, double beat)
    {
        auto p = pat (e, patternName);
        auto* t = ch (e, channelName);
        return p.isValid() && t != nullptr ? song::findNoteAt (p, song::getChannelID (*t), pitch, beat) : juce::ValueTree();
    }

    static te::AutomatableParameter* drumParam (te::Edit& e, const juce::String& channelName, const juce::String& paramID)
    {
        if (auto* t = ch (e, channelName))
            if (auto* inst = song::getInstrument (*t))
                if (auto ap = inst->getAutomatableParameterByID (paramID))
                    return ap.get();

        return nullptr;
    }

    static float effectKnob (te::Edit& e)
    {
        if (auto* t = ch (e, "Synth"))
            for (auto* p : song::getEffects (*t))
                if (auto* ext = dynamic_cast<te::ExternalPlugin*> (p))
                    if (auto* pi = ext->getAudioPluginInstance())
                        if (! pi->getParameters().isEmpty())
                            return pi->getParameters()[0]->getValue();

        return -1.0f;
    }

    static void setEffectKnob (te::Edit& e, float v)
    {
        if (auto* t = ch (e, "Synth"))
            for (auto* p : song::getEffects (*t))
                if (auto* ext = dynamic_cast<te::ExternalPlugin*> (p))
                    if (auto* pi = ext->getAudioPluginInstance())
                        pi->getParameters()[0]->setValueNotifyingHost (v);
    }

    juce::StringArray compiled (collab::Session& s, const juce::String& channelName)
    {
        if (auto* t = ch (*s.getEdit(), channelName))
            if (auto* c = s.getCompiler())
                return c->describeCompiledNotes (*t);

        return {};
    }

    /** A 16-bit stereo WAV: a sine tone, or noise (noise hardly compresses, so it makes a big transfer). */
    static juce::File writeTestWav (juce::File f, double seconds, double freq, bool noise)
    {
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream());
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));

        if (writer == nullptr)
            return f;

        const int n = (int) (seconds * 44100.0);
        juce::AudioBuffer<float> buffer (2, n);
        juce::Random random (1234);

        for (int i = 0; i < n; ++i)
        {
            const float v = noise ? (random.nextFloat() * 2.0f - 1.0f) * 0.3f
                                  : 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / 44100.0);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, noise ? (random.nextFloat() * 2.0f - 1.0f) * 0.3f : v);   // noise: different per side
        }

        writer->writeFromAudioSampleBuffer (buffer, 0, n);
        return f;
    }

    /** True if this PC has the file and its content matches the fingerprint. */
    static bool hasVerified (collab::Session& s, const juce::String& hash)
    {
        auto& store = s.getAssetStore();
        return hash.isNotEmpty() && store.has (hash) && AssetStore::fingerprint (store.getFile (hash)) == hash;
    }

    static juce::ValueTree audioClipOf (te::Edit& e, const juce::String& hash)
    {
        for (auto& c : song::getClips (e))
            if (song::isAudioClip (c) && c[song::ids::asset].toString() == hash)
                return c;

        return {};
    }

    static int chatCount (collab::Session& s, const juce::String& text)
    {
        int n = 0;

        for (auto& line : s.getChat())
            if (line.text == text)
                ++n;

        return n;
    }

    static juce::StringArray peopleNames (collab::Session& s)
    {
        juce::StringArray names;

        for (auto& p : s.getPeople())
            names.add (p.name + (p.isHost ? "*" : "") + (p.isMe ? "(me)" : ""));

        return names;
    }

    /** Every channel's compiled notes, as one list (to compare what the song plays). */
    juce::StringArray allCompiled (collab::Session& s)
    {
        juce::StringArray all;

        for (auto* t : song::getChannels (*s.getEdit()))
            for (auto& n : compiled (s, t->getName()))
                all.add (t->getName() + ":" + n);

        all.sort (false);
        return all;
    }

    static juce::ValueTree clipOfChannel (te::Edit& e, const juce::String& channelName)
    {
        if (auto* t = ch (e, channelName))
            for (auto& c : song::getClips (e))
                if (song::isAudioClip (c) && c[song::ids::ch].toString() == song::getChannelID (*t))
                    return c;

        return {};
    }

    /** Peak level of an audio file between two times (seconds); -1 if unreadable. */
    static float peakOf (const juce::File& f, double from, double to, double* lengthOut = nullptr)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));

        if (reader == nullptr)
            return -1.0f;

        if (lengthOut != nullptr)
            *lengthOut = (double) reader->lengthInSamples / reader->sampleRate;

        const auto start = (juce::int64) (from * reader->sampleRate);
        const int num = (int) ((to - from) * reader->sampleRate);
        juce::AudioBuffer<float> buffer ((int) reader->numChannels, num);
        reader->read (&buffer, 0, num, start, true, true);
        return buffer.getMagnitude (0, num);
    }

    juce::StringArray compiledAudio (collab::Session& s, const juce::String& channelName)
    {
        if (auto* t = ch (*s.getEdit(), channelName))
            if (auto* c = s.getCompiler())
                return c->describeCompiledAudio (*t);

        return {};
    }

    bool allJoined() const
    {
        return peer1.getMode() == collab::Session::Mode::joined && peer2.getMode() == collab::Session::Mode::joined;
    }

    bool hashesEqual()
    {
        auto h = host.getSync()->computeHash();
        return peer1.getSync() != nullptr && peer2.getSync() != nullptr
                && peer1.getSync()->computeHash() == h && peer2.getSync()->computeHash() == h;
    }

    bool everywhere (std::function<bool (te::Edit&)> pred)
    {
        return pred (H()) && pred (P1()) && pred (P2());
    }

    bool compiledEqualEverywhere()
    {
        for (auto* t : song::getChannels (H()))
        {
            auto a = compiled (host, t->getName());

            if (a != compiled (peer1, t->getName()) || a != compiled (peer2, t->getName()))
                return false;

            auto au = compiledAudio (host, t->getName());

            if (au != compiledAudio (peer1, t->getName()) || au != compiledAudio (peer2, t->getName()))
                return false;
        }

        return true;
    }

    static bool idsUnique (te::Edit& e)
    {
        auto list = te::EditItemID::findAllIDs (e.state);
        std::sort (list.begin(), list.end());
        return std::adjacent_find (list.begin(), list.end()) == list.end();
    }

    void dump (const juce::String& name, te::Edit& e)
    {
        if (dumpDir == juce::File())
            return;

        dumpDir.createDirectory();
        dumpDir.getChildFile (name + "-full.xml").replaceWithText (e.state.toXmlString());
        dumpDir.getChildFile (name + "-shared.xml").replaceWithText (collab::EditSync::filteredCopy (e.state, true).toXmlString());
    }

    static te::AudioTrack* insert (te::Edit& e, const juce::String& name)
    {
        for (auto* t : song::getInserts (e))
            if (t->getName() == name)
                return t;

        return nullptr;
    }

    /** Where the engine on this PC actually sends a track: the insert's name or "Master". */
    static juce::String engineDest (te::Edit& e, te::AudioTrack* t)
    {
        if (t == nullptr)
            return "?";

        auto* d = t->getOutput().getDestinationTrack();
        return d != nullptr ? d->getName() : juce::String ("Master");
    }

    bool engineRoutingMatchesEverywhere()
    {
        for (auto* t : te::getAudioTracks (H()))
        {
            auto find = [t] (te::Edit& e) -> te::AudioTrack*
            {
                for (auto* o : te::getAudioTracks (e))
                    if (o->itemID == t->itemID)
                        return o;
                return nullptr;
            };

            auto a = engineDest (H(), t);

            if (a != engineDest (P1(), find (P1())) || a != engineDest (P2(), find (P2())))
                return false;
        }

        return true;
    }

    /** Renders only the named tracks (by name) over the loop range; returns the peak. */
    float renderPeakOf (te::Edit& e, juce::StringArray trackNames)
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("synctest-render", ".wav");
        juce::BigInteger tracks;
        auto all = te::getAllTracks (e);

        for (int i = 0; i < all.size(); ++i)
            if (trackNames.contains (all[i]->getName()))
                tracks.setBit (i);

        if (! te::Renderer::renderToFile ("test", file, e, e.getTransport().getLoopRange(), tracks, true, true, {}, false))
            return -1.0f;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        float peak = 0.0f;

        if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (file) })
        {
            juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
            peak = buffer.getMagnitude (0, buffer.getNumSamples());
        }

        file.deleteFile();
        return peak;
    }

    float renderPeak (te::Edit& e)
    {
        // Renders the playing pattern offline and returns the loudest sample.
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("synctest-render", ".wav");
        juce::BigInteger tracks;
        tracks.setRange (0, te::getAllTracks (e).size(), true);
        auto range = e.getTransport().getLoopRange();

        if (! te::Renderer::renderToFile ("test", file, e, range, tracks, true, true, {}, false))
            return -1.0f;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        float peak = 0.0f;

        if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (file) })
        {
            juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
            peak = buffer.getMagnitude (0, buffer.getNumSamples());
        }

        file.deleteFile();
        return peak;
    }

    //==============================================================================
    void buildSteps()
    {
        steps.push_back ({ "Host creates the default song (Kick, Clap, Hat, Snare, Synth + a beat) and hosts",
            [this]
            {
                host.createLocalEdit();
                newSongHash = host.getSync()->computeContentHash();   // what the window remembers as "saved"
                host.startHosting (port);
            },
            [this]
            {
                return host.getMode() == collab::Session::Mode::hosting
                    && song::getChannels (H()).size() == 5 && song::getPatterns (H()).size() == 1
                    && compiled (host, "Kick").size() == 4 && compiled (host, "Hat").size() == 4;
            } });

        steps.push_back ({ "An untouched new song still counts as unchanged once the engine has settled (no '*' in the title)",
            {},
            [this] { return host.getSync()->computeContentHash() == newSongHash; },
            4000, 1500 });

        steps.push_back ({ "The default beat makes sound (offline render of the drum synth)",
            {},
            [this] { auto peak = renderPeak (H()); std::cout << "    peak level " << peak << std::endl; return peak > 0.05f; },
            8000, 0, true });

        steps.push_back ({ "Two peers join and receive the song",
            [this] { peer1.join ("127.0.0.1", port); peer2.join ("127.0.0.1", port); },
            [this] { return allJoined() && hashesEqual() && compiledEqualEverywhere(); } });

        steps.push_back ({ "Every drum channel plays its own sound on every PC (host included)",
            {},
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    const char* names[] = { "Kick", "Clap", "Hat", "Snare" };

                    for (int i = 0; i < 4; ++i)
                    {
                        auto* t = ch (e, names[i]);
                        auto* drum = t != nullptr ? dynamic_cast<DrumSynthPlugin*> (song::getInstrument (*t)) : nullptr;

                        if (drum == nullptr || juce::roundToInt (drum->soundParam->getCurrentValue()) != i
                             || drum->soundValue.get() != i)
                            return false;
                    }

                    return true;
                });
            } });

        steps.push_back ({ "Copies stay identical for 4 s with nobody editing (no resyncs needed)",
            [this] { dump ("host-after-join", H()); dump ("peer1-after-join", P1()); },
            [this] { return hashesEqual() && peer1.getResyncCount() == 0 && peer2.getResyncCount() == 0; },
            8000, 4000 });

        //==============================================================================
        // People and chat
        steps.push_back ({ "People: everyone sees the same 3 people (host marked, 'me' marked on each PC)",
            {},
            [this]
            {
                auto h = peopleNames (host), a = peopleNames (peer1), b = peopleNames (peer2);
                const bool ok = h.size() == 3 && a.size() == 3 && b.size() == 3
                    && h[0] == "host*(me)" && a.contains ("peer1(me)") && b.contains ("peer2(me)")
                    && a.contains ("host*") && b.contains ("peer1") && h.contains ("peer2");

                if (ok)
                    std::cout << "    peer 1 sees: " << a.joinIntoString (", ") << std::endl;

                return ok;
            } });

        steps.push_back ({ "People: peer 2 opens Pattern 1 -> host and peer 1 see where peer 2 is",
            [this] { peer2.setViewingPattern (pat (P2(), "Pattern 1")[song::ids::sid].toString()); },
            [this]
            {
                const auto sid = pat (H(), "Pattern 1")[song::ids::sid].toString();

                auto seesPeer2There = [&sid] (collab::Session& s)
                {
                    for (auto& p : s.getPeople())
                        if (p.name == "peer2")
                            return p.viewing == sid;

                    return false;
                };

                return seesPeer2There (host) && seesPeer2There (peer1);
            } });

        steps.push_back ({ "Chat: peer 2 and the host write -> every message reaches everyone exactly once",
            [this]
            {
                peer2.sendChat ("hello from peer 2");
                host.sendChat ("hi, host here");
            },
            [this]
            {
                for (auto* sess : { &host, &peer1, &peer2 })
                    if (chatCount (*sess, "hello from peer 2") != 1 || chatCount (*sess, "hi, host here") != 1)
                        return false;

                // Names come from who really sent it.
                for (auto& line : peer1.getChat())
                    if (line.text == "hello from peer 2" && line.name != "peer2")
                        return false;

                return chatCount (host, "peer2 joined") == 1;
            },
            8000, 600 });

        steps.push_back ({ "Host clicks a step (Kick, step 3) -> lit everywhere and compiled into playback",
            [this] { song::setStep (H(), pat (H(), "Pattern 1"), *ch (H(), "Kick"), 2, true); },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return hasStep (e, "Pattern 1", "Kick", 2); })
                    && compiled (peer2, "Kick").size() == 5 && compiledEqualEverywhere();
            } });

        steps.push_back ({ "Peer 1 draws a note in the piano roll (Synth) -> everywhere",
            [this] { song::addNote (P1(), pat (P1(), "Pattern 1"), song::getChannelID (*ch (P1(), "Synth")), 67, 1.0, 0.5); },
            [this] { return everywhere ([] (te::Edit& e) { return note (e, "Pattern 1", "Synth", 67, 1.0).isValid(); }); } });

        steps.push_back ({ "Peer 2 turns off a step (Kick, step 1) -> off everywhere",
            [this] { song::setStep (P2(), pat (P2(), "Pattern 1"), *ch (P2(), "Kick"), 0, false); },
            [this] { return everywhere ([] (te::Edit& e) { return ! hasStep (e, "Pattern 1", "Kick", 0); }); } });

        steps.push_back ({ "Peer 2 drags that piano-roll note to another time and pitch -> moves everywhere",
            [this]
            {
                auto n = note (P2(), "Pattern 1", "Synth", 67, 1.0);
                n.setProperty (song::ids::p, 69, &P2().getUndoManager());
                n.setProperty (song::ids::b, 2.0, &P2().getUndoManager());
            },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return note (e, "Pattern 1", "Synth", 69, 2.0).isValid()
                                                             && ! note (e, "Pattern 1", "Synth", 67, 1.0).isValid(); });
            } });

        steps.push_back ({ "Peer 1 makes 'Pattern 2' (2 bars), peer 2 adds a Hat step in bar 2 -> everywhere",
            [this] { song::addPattern (P1(), "Pattern 2", 2); },
            [this] { return everywhere ([] (te::Edit& e) { return song::getBars (pat (e, "Pattern 2")) == 2; }); } });

        steps.push_back ({ "  ...and the Hat step in bar 2 arrives everywhere",
            [this] { song::setStep (P2(), pat (P2(), "Pattern 2"), *ch (P2(), "Hat"), 20, true); },
            [this] { return everywhere ([] (te::Edit& e) { return hasStep (e, "Pattern 2", "Hat", 20); }); } });

        steps.push_back ({ "Peer 1 changes the Kick's drum-synth Decay knob -> same value everywhere",
            [this] { drumParam (P1(), "Kick", "decay")->setNormalisedParameter (0.75f, juce::sendNotification); },
            [this]
            {
                auto* a = drumParam (H(), "Kick", "decay");
                auto* c = drumParam (P2(), "Kick", "decay");
                return a && c && std::abs (a->getCurrentNormalisedValue() - 0.75f) < 1.0e-4f
                        && std::abs (c->getCurrentNormalisedValue() - 0.75f) < 1.0e-4f;
            } });

        steps.push_back ({ "Tempo: peer 2 asks for 140 BPM -> host and peer 1 are asked; the song stays at 120 until they agree",
            [this] { peer2.requestTempo (140.0); },
            [this]
            {
                return questionOpen (host) && questionOpen (peer1) && tempoAsked[&peer2].isEmpty()
                    && std::abs (tempoAsked[&host].getLast().bpm - 140.0) < 0.01
                    && std::abs (peer2.getPendingTempo() - 140.0) < 0.01
                    && everywhere ([] (te::Edit& e) { return std::abs (bpmOf (e) - 120.0) < 0.01; });
            },
            8000, 600 });

        steps.push_back ({ "Tempo: host and peer 1 accept -> 140 BPM everywhere, and everyone reads why in the chat",
            [this]
            {
                host.answerTempo (lastQuestion (host), true);
                peer1.answerTempo (lastQuestion (peer1), true);
            },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return std::abs (bpmOf (e) - 140.0) < 0.01; })
                    && peer2.getPendingTempo() == 0.0 && hashesEqual()
                    && chatCount (host, "Tempo changed to 140 BPM (everyone agreed)") == 1
                    && chatCount (peer2, "Tempo changed to 140 BPM (everyone agreed)") == 1;
            } });

        steps.push_back ({ "Tempo: peer 1 asks for 150 BPM -> host and peer 2 are asked",
            [this] { peer1.requestTempo (150.0); },
            [this] { return questionOpen (host) && questionOpen (peer2) && std::abs (peer1.getPendingTempo() - 150.0) < 0.01; } });

        steps.push_back ({ "Tempo: the host declines -> stays at 140; peer 2's question closes; everyone is told who declined",
            [this] { host.answerTempo (lastQuestion (host), false); },
            [this]
            {
                return ! questionOpen (peer2) && peer1.getPendingTempo() == 0.0
                    && everywhere ([] (te::Edit& e) { return std::abs (bpmOf (e) - 140.0) < 0.01; })
                    && chatCount (peer1, "host declined the tempo change to 150 BPM - it stays at 140") == 1;
            },
            8000, 600 });

        steps.push_back ({ "Tempo: the host asks for 100 BPM and nobody answers (2 s to answer here) -> cancelled, stays at 140",
            [this]
            {
                host.setTempoAnswerTimeout (2000);
                host.requestTempo (100.0);
            },
            [this]
            {
                return ! questionOpen (peer1) && ! questionOpen (peer2) && host.getPendingTempo() == 0.0
                    && everywhere ([] (te::Edit& e) { return std::abs (bpmOf (e) - 140.0) < 0.01; })
                    && chatCount (peer2, "Tempo change to 100 BPM cancelled: peer1, peer2 didn't answer in time") == 1;
            },
            8000 });

        steps.push_back ({ "Host turns the Snare volume knob and peer 1 mutes the Clap -> everywhere",
            [this]
            {
                song::getVolumePlugin (*ch (H(), "Snare"))->volParam->setNormalisedParameter (0.5f, juce::sendNotification);
                ch (P1(), "Clap")->setMute (true);
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    return std::abs (song::getVolumePlugin (*ch (e, "Snare"))->volParam->getCurrentNormalisedValue() - 0.5f) < 1.0e-4f
                        && ch (e, "Clap")->isMuted (false);
                });
            } });

        const bool havePlugin = pluginFile.exists();

        steps.push_back ({ "Host adds a third-party VST3 effect to the Synth channel -> loads everywhere",
            [this]
            {
                juce::VST3PluginFormat format;
                juce::OwnedArray<juce::PluginDescription> found;
                format.findAllTypesForFile (found, pluginFile.getFullPathName());

                if (! found.isEmpty())
                {
                    engine.getPluginManager().knownPluginList.addType (*found[0]);
                    song::addEffect (*ch (H(), "Synth"), *found[0]);
                }
            },
            [this]
            {
                // Loaded on every PC, and every PC is watching its knobs (so moves are sent).
                return effectKnob (H()) >= 0 && effectKnob (P1()) >= 0 && effectKnob (P2()) >= 0
                    && host.getSync()->getNumListenedParameters() > 0
                    && peer1.getSync()->getNumListenedParameters() > 0
                    && peer2.getSync()->getNumListenedParameters() > 0;
            },
            15000, 600, ! havePlugin });

        steps.push_back ({ "Knob turned in that plugin's window on the host -> peers follow",
            [this] { setEffectKnob (H(), 0.8f); },
            [this] { return std::abs (effectKnob (P1()) - 0.8f) < 1.0e-4f && std::abs (effectKnob (P2()) - 0.8f) < 1.0e-4f; },
            8000, 0, ! havePlugin });

        steps.push_back ({ "Fast knob drag (200 moves) on peer 2 -> everyone ends on the last value",
            [this] { for (int i = 0; i <= 200; ++i) setEffectKnob (P2(), i / 200.0f * 0.3f); },
            [this] { return std::abs (effectKnob (H()) - 0.3f) < 1.0e-4f && std::abs (effectKnob (P1()) - 0.3f) < 1.0e-4f; },
            8000, 0, ! havePlugin });

        steps.push_back ({ "Peer 2 sets a knob then closes the plugin window (full state sent) -> host has it",
            [this]
            {
                setEffectKnob (P2(), 0.42f);

                if (auto* t = ch (P2(), "Synth"))
                    for (auto* p : song::getEffects (*t))
                    {
                        collab::EditSync::ScopedPluginStateCommit commit;
                        p->edit.flushPluginStateIfNeeded (*p);
                    }
            },
            [this] { return std::abs (effectKnob (H()) - 0.42f) < 1.0e-4f && std::abs (effectKnob (P1()) - 0.42f) < 1.0e-4f; },
            8000, 0, ! havePlugin });

        steps.push_back ({ "All three edit at the same instant (two steps + a new channel) -> everyone converges",
            [this]
            {
                song::setStep (H(),  pat (H(),  "Pattern 1"), *ch (H(),  "Kick"), 6, true);
                song::setStep (P1(), pat (P1(), "Pattern 1"), *ch (P1(), "Kick"), 10, true);
                song::addChannel (P2(), song::InstrumentKind::clap)->setName ("Clap 2");
            },
            [this]
            {
                return hashesEqual()
                    && everywhere ([] (te::Edit& e) { return hasStep (e, "Pattern 1", "Kick", 6) && hasStep (e, "Pattern 1", "Kick", 10)
                                                             && song::getChannels (e).size() == 6 && ch (e, "Clap 2") != nullptr; });
            },
            20000, 3500 });

        steps.push_back ({ "Item IDs are unique (no clashes between peers)",
            {},
            [this] { return idsUnique (H()) && idsUnique (P1()) && idsUnique (P2()); } });

        //==============================================================================
        // Playback is personal; sharing makes the others follow
        steps.push_back ({ "Playback is personal: peer 2 plays Pattern 2 -> only peer 2 plays",
            [this]
            {
                host.setTempoAnswerTimeout (30000);
                peer1.setViewingPattern (pat (P1(), "Pattern 1")[song::ids::sid].toString());
                peer2.setPlayingPattern (pat (P2(), "Pattern 2")[song::ids::sid].toString());
                peer2.setPlaying (true, 0.0);
            },
            [this]
            {
                auto sid = pat (H(), "Pattern 2")[song::ids::sid].toString();
                return P2().getTransport().isPlaying() && peer2.getPlayingPattern() == sid
                    && ! H().getTransport().isPlaying() && ! P1().getTransport().isPlaying()
                    && host.getPlayingPattern() != sid && peer1.getPlayingPattern() != sid;
            },
            4000, 800 });

        steps.push_back ({ "Peer 1 switches to SONG and moves its playhead to beat 2 -> only on peer 1; the host turns Focus on",
            [this]
            {
                peer1.setSongMode (true);
                peer1.seek (timeOfBeat (P1(), 2.0));
                host.setFocus (true);
            },
            [this]
            {
                bool hostInFocus = false;

                for (auto& p : peer1.getPeople())
                    if (p.name == "host")
                        hostInFocus = p.focus;

                return peer1.isSongMode() && ! host.isSongMode() && ! peer2.isSongMode()
                    && std::abs (beatOf (P1()) - 2.0) < 0.01 && hostInFocus;
            },
            4000, 800 });

        steps.push_back ({ "Peer 2 shares -> peer 1 follows into Pattern 2, playing in sync; the host (Focus) is left alone",
            [this] { shareError = peer2.startSharing(); },
            [this]
            {
                auto sid = pat (H(), "Pattern 2")[song::ids::sid].toString();

                if (! (shareError.isEmpty() && peer2.isSharing() && peer1.isFollowingShare()
                        && P1().getTransport().isPlaying() && peer1.getPlayingPattern() == sid && ! peer1.isSongMode()
                        && ! host.isFollowingShare() && ! H().getTransport().isPlaying()
                        && compiled (peer1, "Hat").contains ("60@5.000+0.250*100")))
                    return false;

                // Same spot in the loop (Pattern 2 is 2 bars long).
                const double loop = timeOfBeat (P1(), 8.0);
                auto d = std::fmod (P1().getTransport().getPosition().inSeconds() - P2().getTransport().getPosition().inSeconds() + loop, loop);
                shareOffset = juce::jmin (d, loop - d);
                std::cout << "    peer 1 is " << juce::roundToInt (shareOffset * 1000.0) << " ms from peer 2" << std::endl;

                return shareOffset < 0.25
                    && chatCount (host, "peer2 is sharing their playback (not following: host (Focus))") == 1
                    && chatCount (peer1, "peer2 is sharing their playback (not following: host (Focus))") == 1;
            },
            6000 });

        steps.push_back ({ "  ...peer 1 (looking at Pattern 1) is asked whether to go to Pattern 2",
            {},
            [this] { return patternOffers[&peer1].contains (pat (P1(), "Pattern 2")[song::ids::sid].toString()); } });

        steps.push_back ({ "While following, peer 1 can't change the playback (Stop, PAT/SONG, moving the playhead are ignored)",
            [this]
            {
                peer1.setPlaying (false, 0.0);
                peer1.setSongMode (true);
                peer1.seek (timeOfBeat (P1(), 6.0));
            },
            [this]
            {
                return P1().getTransport().isPlaying() && ! peer1.isSongMode()
                    && P2().getTransport().isPlaying() && ! peer2.isSongMode();
            },
            4000, 800 });

        steps.push_back ({ "Peer 2 jumps to beat 3 -> peer 1 jumps there too",
            [this] { peer2.seek (timeOfBeat (P2(), 3.0)); },
            [this]
            {
                return beatOf (P1()) >= 2.99 && beatOf (P1()) < 4.5 && beatOf (P2()) >= 2.99 && beatOf (P2()) < 4.5;
            },
            4000 });

        steps.push_back ({ "What peer 1 plays is identical to peer 2 (compiled notes of every channel)",
            {},
            [this] { return compiledEqual (peer1, peer2); } });

        steps.push_back ({ "Note preview (guide note) plays and stops on the selected channel, locally only",
            [this]
            {
                auto* synth = ch (P1(), "Synth");
                synth->playGuideNote (64, te::MidiChannel (1), 100, true, true, false);
                synth->turnOffGuideNotes();
            },
            [this] { return hashesEqual(); } });

        steps.push_back ({ "Peer 1 leaves the share -> back where it was (SONG, stopped, beat 2); everyone reads it in the chat",
            [this] { peer1.leaveShare(); },
            [this]
            {
                return ! peer1.isFollowingShare() && ! P1().getTransport().isPlaying() && peer1.isSongMode()
                    && std::abs (beatOf (P1()) - 2.0) < 0.01 && P2().getTransport().isPlaying()
                    && chatCount (host, "peer1 left peer2's playback") == 1 && chatCount (peer2, "peer1 left peer2's playback") == 1;
            } });

        steps.push_back ({ "Peer 1 joins again -> follows peer 2 again (playing Pattern 2)",
            [this] { peer1.joinShare(); },
            [this]
            {
                return peer1.isFollowingShare() && P1().getTransport().isPlaying() && ! peer1.isSongMode()
                    && peer1.getPlayingPattern() == pat (P1(), "Pattern 2")[song::ids::sid].toString()
                    && chatCount (peer2, "peer1 joined peer2's playback") == 1;
            } });

        steps.push_back ({ "The host turns Focus off -> joins the running share",
            [this] { host.setFocus (false); },
            [this]
            {
                return host.isFollowingShare() && H().getTransport().isPlaying()
                    && host.getPlayingPattern() == pat (H(), "Pattern 2")[song::ids::sid].toString();
            } });

        steps.push_back ({ "Peer 2 presses Stop -> everyone following stops",
            [this] { peer2.setPlaying (false, 0.0); },
            [this] { return ! H().getTransport().isPlaying() && ! P1().getTransport().isPlaying() && ! P2().getTransport().isPlaying(); },
            4000 });

        steps.push_back ({ "Peer 2 stops sharing -> host and peer 1 are back to their own playback, as before",
            [this] { peer2.stopSharing(); },
            [this]
            {
                return ! peer2.isShareRunning() && ! host.isFollowingShare() && ! peer1.isFollowingShare()
                    && peer1.isSongMode() && std::abs (beatOf (P1()) - 2.0) < 0.01
                    && ! host.isSongMode() && host.getPlayingPattern() == pat (H(), "Pattern 1")[song::ids::sid].toString()
                    && chatCount (host, "peer2 stopped sharing") == 1;
            } });

        steps.push_back ({ "Playlist: host and peer 1 place clips at the same instant -> both everywhere",
            [this]
            {
                song::addClip (H(),  pat (H(),  "Pattern 1")[song::ids::sid].toString(), 1, 4.0);   // lane 2, bar 2
                song::addClip (P1(), pat (P1(), "Pattern 2")[song::ids::sid].toString(), 0, 8.0);   // lane 1, bar 3
            },
            [this]
            {
                return hashesEqual() && everywhere ([] (te::Edit& e) { return song::getClips (e).size() == 3; });
            },
            8000, 1600 });

        steps.push_back ({ "Playlist: peer 2 moves the Pattern 2 clip to bar 5 / lane 3 and shortens it to 1 bar",
            [this]
            {
                for (auto& c : song::getClips (P2()))
                    if (c[song::ids::pattern].toString() == pat (P2(), "Pattern 2")[song::ids::sid].toString())
                    {
                        c.setProperty (song::ids::start, 16.0, &P2().getUndoManager());
                        c.setProperty (song::ids::lane, 2, &P2().getUndoManager());
                        c.setProperty (song::ids::length, 4.0, &P2().getUndoManager());
                    }
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    for (auto& c : song::getClips (e))
                        if (c[song::ids::pattern].toString() == pat (e, "Pattern 2")[song::ids::sid].toString())
                            return (double) c[song::ids::start] == 16.0 && (int) c[song::ids::lane] == 2
                                    && song::getClipLength (e, c) == 4.0;
                    return false;
                }) && std::abs (song::getSongLengthBeats (H()) - 20.0) < 1.0e-6;
            } });

        steps.push_back ({ "Peer 1 shares in SONG mode and plays -> everyone plays the arrangement",
            [this]
            {
                peer1.setSongMode (true);
                shareError = peer1.startSharing();
                peer1.setPlaying (true, 0.0);
            },
            [this]
            {
                // Pattern 1 placed at bar 2 (beat 4): its Kick step 3 (beat 0.5) plays at beat 4.5.
                return host.isSongMode() && peer2.isSongMode()
                    && H().getTransport().isPlaying() && P2().getTransport().isPlaying()
                    && compiled (host, "Kick").contains ("60@4.500+0.250*100")
                    && compiledEqualEverywhere();
            },
            4000 });

        steps.push_back ({ "SONG mode: the shortened Pattern 2 clip only plays its first bar",
            {},
            [this]
            {
                // Pattern 2's Hat step 21 (beat 5) is in its 2nd bar, cut off by the 1-bar clip.
                for (auto& n : compiled (host, "Hat"))
                    if (n.startsWith ("60@21.000"))
                        return false;

                return std::abs (H().getTransport().getLoopRange().getEnd().inSeconds()
                                 - H().tempoSequence.toTime (te::BeatPosition::fromBeats (20.0)).inSeconds()) < 1.0e-6;
            } });

        steps.push_back ({ "Playlist: host deletes the lane-2 clip; peer 2 deletes Pattern 2 (its clip goes too)",
            [this]
            {
                for (auto& c : song::getClips (H()))
                    if ((int) c[song::ids::lane] == 1)
                        song::removeClip (H(), c);

                song::removePattern (P2(), pat (P2(), "Pattern 2"));
            },
            [this]
            {
                return hashesEqual()
                    && everywhere ([] (te::Edit& e) { return song::getClips (e).size() == 1 && ! pat (e, "Pattern 2").isValid(); })
                    && compiledEqualEverywhere();
            },
            8000, 1600 });

        steps.push_back ({ "Peer 1 stops and ends its share -> everyone back to their own playback, stopped",
            [this] { peer1.setPlaying (false, 0.0); peer1.stopSharing(); },
            [this]
            {
                return ! host.isShareRunning() && ! peer2.isShareRunning()
                    && ! host.isFollowingShare() && ! peer2.isFollowingShare()
                    && ! H().getTransport().isPlaying() && ! P1().getTransport().isPlaying() && ! P2().getTransport().isPlaying();
            },
            4000 });

        steps.push_back ({ "(each PC picks PAT mode and Pattern 1 on its own, for the next tests)",
            [this]
            {
                for (auto* sess : { &host, &peer1, &peer2 })
                {
                    sess->setSongMode (false);
                    sess->setPlayingPattern (pat (*sess->getEdit(), "Pattern 1")[song::ids::sid].toString());
                }
            },
            [this] { return ! host.isSongMode() && ! peer1.isSongMode() && ! peer2.isSongMode() && compiledEqualEverywhere(); } });

        steps.push_back ({ "Mixer: host makes insert 'Drums' and routes Kick + Clap into it -> applied on every PC",
            [this]
            {
                auto* ins = song::addInsert (H(), "Drums");
                song::setRoute (H(), *ch (H(), "Kick"), ins->itemID.toString());
                song::setRoute (H(), *ch (H(), "Clap"), ins->itemID.toString());
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    return song::getInserts (e).size() == 1 && insert (e, "Drums") != nullptr
                        && engineDest (e, ch (e, "Kick")) == "Drums" && engineDest (e, ch (e, "Clap")) == "Drums"
                        && engineDest (e, ch (e, "Hat")) == "Master";
                }) && hashesEqual();
            },
            8000, 600 });

        steps.push_back ({ "Mixer: the Kick really plays through the insert (insert fader down = Kick silent)",
            [this] { host.getRouter()->applyNow(); },
            [this]
            {
                // Rendering specific tracks ignores mute on purpose (like freezing),
                // so use the insert's volume fader, which the render does respect.
                auto vol = song::getVolumePlugin (*insert (H(), "Drums"))->volParam;
                const float original = vol->getCurrentValue();
                const float open = renderPeakOf (H(), { "Kick", "Drums" });
                vol->setNormalisedParameter (0.0f, juce::dontSendNotification);
                const float closed = renderPeakOf (H(), { "Kick", "Drums" });
                vol->setParameter (original, juce::dontSendNotification);
                std::cout << "    kick through insert: peak " << open << ", insert fader down: " << closed << std::endl;
                return open > 0.05f && closed < 0.001f;
            } });

        steps.push_back ({ "Mixer: peer 1 adds a built-in Compressor to 'Drums' and a VST3 effect to the master",
            [this]
            {
                auto* ins = insert (P1(), "Drums");
                song::putEffect (P1(), ins, song::createEffect (P1(), te::CompressorPlugin::xmlTypeName, nullptr), -1);

                auto types = engine.getPluginManager().knownPluginList.getTypes();

                for (auto& d : types)
                    if (d.name == "SyncTestGain")
                        song::putEffect (P1(), nullptr, song::createEffect (P1(), {}, &d), -1);
            },
            [this]
            {
                const bool vst = pluginFile.exists();

                return everywhere ([vst] (te::Edit& e)
                {
                    auto fx = song::getInsertEffects (*insert (e, "Drums"));
                    auto master = song::getMasterEffects (e);
                    return fx.size() == 1 && dynamic_cast<te::CompressorPlugin*> (fx[0]) != nullptr
                        && (! vst || (master.size() == 1 && master[0]->getName() == "SyncTestGain"));
                });
            },
            15000, 600 });

        steps.push_back ({ "Mixer: master volume (host) and the insert's pan (peer 2) -> same everywhere",
            [this]
            {
                H().getMasterVolumePlugin()->volParam->setNormalisedParameter (0.6f, juce::sendNotification);
                song::getVolumePlugin (*insert (P2(), "Drums"))->panParam->setNormalisedParameter (0.2f, juce::sendNotification);
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    return std::abs (e.getMasterVolumePlugin()->volParam->getCurrentNormalisedValue() - 0.6f) < 1.0e-4f
                        && std::abs (song::getVolumePlugin (*insert (e, "Drums"))->panParam->getCurrentNormalisedValue() - 0.2f) < 1.0e-4f;
                });
            } });

        steps.push_back ({ "Mixer: a routing loop is refused (Drums -> Bus is fine, Bus -> Drums is not)",
            [this]
            {
                auto* bus = song::addInsert (H(), "Bus");
                song::setRoute (H(), *insert (H(), "Drums"), bus->itemID.toString());
                loopRefused = ! song::setRoute (H(), *bus, insert (H(), "Drums")->itemID.toString());
            },
            [this]
            {
                return loopRefused
                    && everywhere ([] (te::Edit& e) { return engineDest (e, insert (e, "Drums")) == "Bus"
                                                             && engineDest (e, insert (e, "Bus")) == "Master"; });
            },
            8000, 600 });

        steps.push_back ({ "Mixer: peer 1 deletes 'Bus' while the host routes Snare into it -> all end up on master",
            [this]
            {
                song::setRoute (H(), *ch (H(), "Snare"), insert (H(), "Bus")->itemID.toString());
                song::removeInsert (P1(), *insert (P1(), "Bus"));
            },
            [this]
            {
                return hashesEqual()
                    && everywhere ([] (te::Edit& e)
                       {
                           return insert (e, "Bus") == nullptr
                               && engineDest (e, ch (e, "Snare")) == "Master" && engineDest (e, insert (e, "Drums")) == "Master"
                               && engineDest (e, ch (e, "Kick")) == "Drums";
                       })
                    && engineRoutingMatchesEverywhere();
            },
            8000, 1600 });

        //==============================================================================
        // Solo (personal), sends (shared), metronome (personal)
        steps.push_back ({ "Solo: peer 1 solos the Kick -> on peer 1 only the Kick (through Drums) sounds; the others hear everything; the song didn't change",
            [this] { peer1.toggleSolo (song::getChannelID (*ch (P1(), "Kick")), false); },
            [this]
            {
                return outputOf (P1(), ch (P1(), "Hat")) == "(none)" && outputOf (P1(), ch (P1(), "Clap")) == "(none)"
                    && outputOf (P1(), ch (P1(), "Synth")) == "(none)" && engineDest (P1(), ch (P1(), "Kick")) == "Drums"
                    && peer1.isSilencedBySolo (song::getChannelID (*ch (P1(), "Hat")))
                    && outputOf (H(), ch (H(), "Hat")) != "(none)" && outputOf (P2(), ch (P2(), "Clap")) != "(none)"
                    && hashesEqual();
            },
            8000, 600 });

        steps.push_back ({ "Solo: peer 1 un-solos the Kick and solos insert 'Drums' -> Kick + Clap sound there, Hat/Snare/Synth don't",
            [this]
            {
                peer1.toggleSolo (song::getChannelID (*ch (P1(), "Kick")), false);
                peer1.toggleSolo (insert (P1(), "Drums")->itemID.toString(), true);
            },
            [this]
            {
                return engineDest (P1(), ch (P1(), "Kick")) == "Drums" && engineDest (P1(), ch (P1(), "Clap")) == "Drums"
                    && outputOf (P1(), ch (P1(), "Hat")) == "(none)" && outputOf (P1(), ch (P1(), "Snare")) == "(none)"
                    && outputOf (P1(), insert (P1(), "Drums")) != "(none)" && hashesEqual();
            } });

        steps.push_back ({ "Solo while sharing: peer 1 shares -> the host and peer 2 hear peer 1's solo too",
            [this] { shareError = peer1.startSharing(); },
            [this]
            {
                return shareError.isEmpty() && host.isFollowingShare() && peer2.isFollowingShare()
                    && outputOf (H(), ch (H(), "Hat")) == "(none)" && outputOf (P2(), ch (P2(), "Synth")) == "(none)"
                    && engineDest (H(), ch (H(), "Kick")) == "Drums";
            } });

        steps.push_back ({ "Peer 1 clears its solo and stops sharing -> everyone hears everything again, routing as before",
            [this] { peer1.clearSolo(); peer1.stopSharing(); },
            [this]
            {
                return ! host.isFollowingShare() && ! peer2.isFollowingShare() && ! peer1.isSharing()
                    && everywhere ([] (te::Edit& e) { return outputOf (e, ch (e, "Hat")) != "(none)" && engineDest (e, ch (e, "Kick")) == "Drums"; })
                    && engineRoutingMatchesEverywhere();
            } });

        steps.push_back ({ "Sends: peer 2 makes insert 'Reverb' and sends 'Drums' to it at -6 dB -> everywhere; each PC builds the send itself",
            [this]
            {
                auto* reverb = song::addInsert (P2(), "Reverb");
                song::addSend (P2(), insert (P2(), "Drums")->itemID.toString(), reverb->itemID.toString(), -6.0);
            },
            [this]
            {
                auto ok = everywhere ([] (te::Edit& e)
                {
                    auto sends = song::getSends (e);
                    auto* drums = insert (e, "Drums");
                    auto* reverb = insert (e, "Reverb");

                    if (sends.size() != 1 || drums == nullptr || reverb == nullptr
                         || std::abs (song::getSendLevel (sends[0]) + 6.0) > 1.0e-6
                         || countGenerated (drums, true) != 1 || countGenerated (reverb, false) != 1)
                        return false;

                    for (auto* p : drums->pluginList.getPlugins())
                        if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                            return std::abs (send->getGainDb() + 6.0f) < 0.05f;

                    return false;
                });

                // The engine's send/return plugins are never part of the shared song.
                const bool notShared = ! collab::EditSync::filteredCopy (H().state, false).toXmlString().contains ("auxsend")
                                    && song::getInsertEffects (*insert (H(), "Drums")).size() == 1;
                return ok && notShared && hashesEqual();
            },
            8000, 600 });

        steps.push_back ({ "Sends: loops are refused (Reverb can't send or route back into Drums)",
            {},
            [this]
            {
                const auto drums = insert (H(), "Drums")->itemID.toString(), reverb = insert (H(), "Reverb")->itemID.toString();
                return ! song::canSendTo (H(), reverb, drums) && ! song::canRouteTo (H(), *insert (H(), "Reverb"), drums)
                    && ! song::canSendTo (H(), drums, reverb)        // already sending there
                    && song::canRouteTo (H(), *insert (H(), "Drums"), reverb);
            } });

        steps.push_back ({ "Sends: the Kick really reaches 'Reverb' through the send (Reverb fader down = quieter)",
            [this] { host.getRouter()->applyNow(); },
            [this]
            {
                auto vol = song::getVolumePlugin (*insert (H(), "Reverb"))->volParam;
                const float original = vol->getCurrentValue();
                sendOpen = renderPeakOf (H(), { "Kick", "Drums", "Reverb" });
                vol->setNormalisedParameter (0.0f, juce::dontSendNotification);
                sendClosed = renderPeakOf (H(), { "Kick", "Drums", "Reverb" });
                vol->setParameter (original, juce::dontSendNotification);
                std::cout << "    kick with the send: peak " << sendOpen << ", Reverb fader down: " << sendClosed << std::endl;
                return sendOpen > sendClosed * 1.2f && sendClosed > 0.05f;
            } });

        steps.push_back ({ "Sends: the host turns the send down to -20 dB, peer 1 moves the Compressor in 'Drums' -> same everywhere",
            [this]
            {
                song::setSendLevel (H(), song::getSends (H())[0], -20.0);
                song::moveEffect (*song::getInsertEffects (*insert (P1(), "Drums"))[0], 1);   // nothing to pass: stays put
            },
            [this]
            {
                return hashesEqual() && everywhere ([] (te::Edit& e)
                {
                    for (auto* p : insert (e, "Drums")->pluginList.getPlugins())
                        if (auto* send = dynamic_cast<te::AuxSendPlugin*> (p))
                            return std::abs (send->getGainDb() + 20.0f) < 0.05f
                                && song::getInsertEffects (*insert (e, "Drums")).size() == 1;

                    return false;
                });
            },
            8000, 600 });

        steps.push_back ({ "Sends: peer 1 deletes 'Reverb' -> its send goes too, and so do the engine's send/return everywhere",
            [this] { song::removeInsert (P1(), *insert (P1(), "Reverb")); },
            [this]
            {
                return hashesEqual() && everywhere ([] (te::Edit& e)
                {
                    return insert (e, "Reverb") == nullptr && song::getSends (e).isEmpty()
                        && countGenerated (insert (e, "Drums"), true) == 0;
                });
            },
            8000, 600 });

        steps.push_back ({ "Metronome: peer 2 turns it on -> only on peer 2; the song didn't change",
            [this] { peer2.setMetronome (true); },
            [this] { return P2().clickTrackEnabled.get() && ! H().clickTrackEnabled.get() && ! P1().clickTrackEnabled.get() && hashesEqual(); },
            8000, 600 });

        //==============================================================================
        // Automation clips (shared; each PC plays them in SONG mode)
        steps.push_back ({ "Automation: peer 1 makes an automation clip for the Kick's volume (a ramp up across bar 1, track 10) -> everywhere",
            [this]
            {
                auto* vp = song::getVolumePlugin (*ch (P1(), "Kick"));
                auto clip = song::addAutomationClip (P1(), vp->itemID.toString(), "volume", 9, 0.0, 4.0, 0.0f);
                autoSid = clip[song::ids::sid].toString();
                auto points = song::getAutomationPoints (clip);
                song::moveAutomationPoint (P1(), points.getLast(), 4.0, 1.0f);
            },
            [this]
            {
                return hashesEqual() && everywhere ([this] (te::Edit& e)
                {
                    auto clip = autoClip (e);
                    auto points = song::getAutomationPoints (clip);
                    return song::isAutomationClip (clip) && points.size() == 2
                        && (float) points.getLast()[song::ids::v] == 1.0f
                        && song::findAutomationTarget (e, clip) == kickVolume (e)
                        && song::describeAutomationTarget (e, clip) == "Kick - Volume";
                });
            },
            8000, 600 });

        steps.push_back ({ "Automation plays in SONG mode: the host and peer 2 switch to SONG and build the Kick's volume curve themselves; peer 1 (PAT) has none",
            [this] { host.setSongMode (true); peer2.setSongMode (true); },
            [this]
            {
                auto h = host.getCompiler()->describeCompiledAutomation (*kickVolume (H()));
                auto p2 = peer2.getCompiler()->describeCompiledAutomation (*kickVolume (P2()));
                const bool local = (bool) kickVolume (H())->getCurve().state[song::ids::generated]
                                && ! collab::EditSync::filteredCopy (H().state, false).toXmlString().contains ("AUTOMATIONCURVE");

                if (h.size() >= 2)
                    std::cout << "    host's curve: " << h.joinIntoString (", ") << std::endl;

                return h.size() >= 2 && h == p2 && kickVolume (P1())->getCurve().getNumPoints() == 0 && local && hashesEqual();
            },
            8000, 600 });

        steps.push_back ({ "Automation: the host's Kick volume follows the curve as the song plays; nothing is sent; the copies stay identical",
            [this]
            {
                sentBeforeAutomation = host.getSync()->getStats().sent + host.getSync()->getStats().paramsSent;
                resyncsBefore = peer1.getResyncCount() + peer2.getResyncCount();
                auto* vol = kickVolume (H());

                vol->updateToFollowCurve (te::TimePosition::fromSeconds (timeOfBeat (H(), 0.5)));
                autoEarly = vol->getCurrentNormalisedValue();
                vol->updateToFollowCurve (te::TimePosition::fromSeconds (timeOfBeat (H(), 3.5)));
                autoLate = vol->getCurrentNormalisedValue();
            },
            [this]
            {
                const int sent = host.getSync()->getStats().sent + host.getSync()->getStats().paramsSent;
                std::cout << "    kick volume at beat 0.5: " << autoEarly << ", at beat 3.5: " << autoLate
                          << "; ops sent meanwhile: " << (sent - sentBeforeAutomation) << std::endl;
                return std::abs (autoEarly - 0.125f) < 0.02f && std::abs (autoLate - 0.875f) < 0.02f
                    && sent == sentBeforeAutomation && hashesEqual()
                    && peer1.getResyncCount() + peer2.getResyncCount() == resyncsBefore;
            },
            8000, 2000 });

        steps.push_back ({ "Automation: peer 2 adds a point (beat 2, 50%) -> everywhere, and the SONG-mode PCs rebuild the curve",
            [this] { song::addAutomationPoint (P2(), autoClip (P2()), 2.0, 0.5f); },
            [this]
            {
                auto h = host.getCompiler()->describeCompiledAutomation (*kickVolume (H()));
                return hashesEqual() && everywhere ([this] (te::Edit& e) { return song::getAutomationPoints (autoClip (e)).size() == 3; })
                    && h == peer2.getCompiler()->describeCompiledAutomation (*kickVolume (P2()))
                    && h.joinIntoString (",").contains (juce::String (timeOfBeat (H(), 2.0), 3) + "=0.500");
            },
            8000, 600 });

        steps.push_back ({ "Automation is audible: rendering the Kick on the host, the kicks early in the bar are much quieter than the late ones",
            {},
            [this]
            {
                autoQuiet = renderPeakBetween (H(), { "Kick", "Drums" }, 0.4, 1.2);
                autoLoud = renderPeakBetween (H(), { "Kick", "Drums" }, 2.9, 3.7);
                std::cout << "    kick peak early in the bar " << autoQuiet << ", late " << autoLoud << std::endl;
                return autoLoud > 0.05f && autoQuiet < autoLoud * 0.5f;
            } });

        steps.push_back ({ "Automation: peer 1 deletes the clip -> gone everywhere, and nobody's Kick volume has a curve any more",
            [this] { song::removeClip (P1(), autoClip (P1())); },
            [this]
            {
                return hashesEqual() && everywhere ([this] (te::Edit& e) { return ! autoClip (e).isValid(); })
                    && kickVolume (H())->getCurve().getNumPoints() == 0 && kickVolume (P2())->getCurve().getNumPoints() == 0;
            },
            8000, 600 });

        steps.push_back ({ "Back in PAT mode, the host moves the Kick's volume by hand -> sent to everyone again",
            [this]
            {
                host.setSongMode (false);
                peer2.setSongMode (false);
                kickVolume (H())->setNormalisedParameter (0.3f, juce::sendNotification);
            },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return std::abs (kickVolume (e)->getCurrentNormalisedValue() - 0.3f) < 1.0e-4f; })
                    && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        //==============================================================================
        // Audio clips and file sharing
        steps.push_back ({ "Audio: host imports a 2 s WAV -> new audio channel + clip; the file reaches both peers (fingerprint checked)",
            [this]
            {
                auto r = host.getAssetStore().importAudioFile (toneFile);
                toneHash = r.hash;
                std::cout << "    imported: " << r.hash << " " << r.bytes << " bytes, " << r.lengthSeconds << " s "
                          << (r.error.isNotEmpty() ? r.error : juce::String()) << std::endl;

                if (r.ok())
                    song::addImportedAudio (H(), r, "tone.wav", 2, 0.0);
            },
            [this]
            {
                return toneHash.isNotEmpty()
                    && everywhere ([this] (te::Edit& e)
                       {
                           auto* t = ch (e, "tone");
                           return t != nullptr && song::isAudioChannel (*t) && audioClipOf (e, toneHash).isValid()
                               && std::abs (song::getAssetSeconds (e, toneHash) - 2.0) < 0.01;
                       })
                    && hasVerified (peer1, toneHash) && hasVerified (peer2, toneHash) && hashesEqual();
            },
            15000 });

        steps.push_back ({ "Audio: everyone switches to SONG mode -> the audio clip is put into playback the same way on every PC",
            [this] { for (auto* sess : { &host, &peer1, &peer2 }) sess->setSongMode (true); },
            [this]
            {
                auto a = compiledAudio (host, "tone");
                return peer1.isSongMode() && peer2.isSongMode() && a.size() == 1 && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "Audio: the received file really plays (offline render of peer 1's audio channel)",
            {},
            [this]
            {
                const float peak = renderPeakOf (P1(), { "tone" });
                std::cout << "    peer 1 audio channel peak " << peak << std::endl;
                return peak > 0.2f;
            } });

        steps.push_back ({ "Audio: peer 2 imports a file -> the host fetches it from peer 2 and passes it on to peer 1",
            [this]
            {
                auto r = peer2.getAssetStore().importAudioFile (voiceFile);
                voiceHash = r.hash;

                if (r.ok())
                    song::addImportedAudio (P2(), r, "voice.wav", 3, 4.0);
            },
            [this]
            {
                return voiceHash.isNotEmpty()
                    && everywhere ([this] (te::Edit& e) { return ch (e, "voice") != nullptr && audioClipOf (e, voiceHash).isValid(); })
                    && hasVerified (host, voiceHash) && hasVerified (peer1, voiceHash)
                    && hashesEqual() && compiledEqualEverywhere();
            },
            15000 });

        steps.push_back ({ "Audio: host imports a 60 s file (about 10 MB) with its upload capped at 3 MB/s; the peers start receiving it",
            [this]
            {
                host.setUploadLimit (3 * 1024 * 1024);   // like a home internet upload

                auto r = host.getAssetStore().importAudioFile (bigFile);
                bigHash = r.hash;
                std::cout << "    big file: " << juce::File::descriptionOfSizeInBytes (r.bytes) << " as FLAC" << std::endl;

                if (r.ok())
                    song::addImportedAudio (H(), r, "big.wav", 4, 8.0);
            },
            [this]
            {
                auto receiving = [this] (collab::Session& s) { return s.getAssetStore().isDownloading (bigHash); };
                return bigHash.isNotEmpty() && receiving (peer1) && receiving (peer2);
            },
            15000 });

        steps.push_back ({ "Audio: in the middle of that transfer, a step click still reaches both peers within 1.5 s",
            [this]
            {
                bigStillTransferring = peer1.getAssetStore().isDownloading (bigHash) && peer2.getAssetStore().isDownloading (bigHash);
                song::setStep (H(), pat (H(), "Pattern 1"), *ch (H(), "Hat"), 9, true);
            },
            [this]
            {
                if (! (hasStep (P1(), "Pattern 1", "Hat", 9) && hasStep (P2(), "Pattern 1", "Hat", 9)))
                    return false;

                // The step must have overtaken the file: both are still receiving it.
                const double p1 = peer1.getAssetStore().getProgress (bigHash), p2 = peer2.getAssetStore().getProgress (bigHash);
                std::cout << "    step arrived while the file was at " << juce::roundToInt (p1 * 100) << "% / "
                          << juce::roundToInt (p2 * 100) << "% on the peers" << std::endl;
                return bigStillTransferring && p1 < 1.0 && p2 < 1.0;
            },
            1500 });

        steps.push_back ({ "Audio: the 60 s file arrives complete and verified on both peers",
            {},
            [this] { return hasVerified (peer1, bigHash) && hasVerified (peer2, bigHash) && compiledEqualEverywhere(); },
            40000 });

        steps.push_back ({ "Audio: peer 1 moves + trims the start of the tone clip, host trims the end of the voice clip -> same everywhere",
            [this]
            {
                host.setUploadLimit (0);
                auto* um1 = &P1().getUndoManager();
                auto tone = audioClipOf (P1(), toneHash);
                tone.setProperty (song::ids::start, 2.0, um1);
                tone.setProperty (song::ids::offset, 0.5, um1);
                tone.setProperty (song::ids::duration, 1.0, um1);

                audioClipOf (H(), voiceHash).setProperty (song::ids::duration, 0.5, &H().getUndoManager());
            },
            [this]
            {
                return hashesEqual() && compiledEqualEverywhere()
                    && everywhere ([this] (te::Edit& e)
                       {
                           auto tone = audioClipOf (e, toneHash);
                           auto voice = audioClipOf (e, voiceHash);
                           return std::abs ((double) tone[song::ids::start] - 2.0) < 1.0e-9
                               && std::abs ((double) tone[song::ids::offset] - 0.5) < 1.0e-9
                               && std::abs ((double) tone[song::ids::duration] - 1.0) < 1.0e-9
                               && std::abs ((double) voice[song::ids::duration] - 0.5) < 1.0e-9;
                       })
                    && compiledAudio (peer2, "tone").joinIntoString (",").contains (">0.500");
            },
            8000, 600 });

        steps.push_back ({ "Audio: peer 2 deletes the tone clip, then places it again from its channel, off the grid (beat 1.37)",
            [this]
            {
                song::removeClip (P2(), audioClipOf (P2(), toneHash));
                P2().getUndoManager().beginNewTransaction();
                song::placeAudioChannel (P2(), *ch (P2(), "tone"), 5, 1.37);
            },
            [this]
            {
                return hashesEqual() && compiledEqualEverywhere()
                    && everywhere ([this] (te::Edit& e)
                       {
                           auto tone = audioClipOf (e, toneHash);
                           return song::getChannelAsset (e, *ch (e, "tone")) == toneHash
                               && std::abs ((double) tone[song::ids::start] - 1.37) < 1.0e-9
                               && (int) tone[song::ids::lane] == 5
                               && (double) tone[song::ids::offset] == 0.0
                               && std::abs ((double) tone[song::ids::duration] - 2.0) < 0.01;
                       });
            },
            8000, 600 });

        steps.push_back ({ "Audio: a damaged transfer is rejected (fingerprint check) and never used",
            {},
            [this]
            {
                auto& store = peer1.getAssetStore();
                const juce::String fake ("0123456789abcdef0123456789abcdef");
                juce::MemoryBlock junk ("not the right audio", 19);
                const bool ignoredMidway = store.receiveChunk (fake, 5, 19, junk, 999) == AssetStore::ChunkResult::ignored;
                const bool rejected = store.receiveChunk (fake, 0, 19, junk, 999) == AssetStore::ChunkResult::failed;
                return ignoredMidway && rejected && ! store.has (fake) && ! store.isDownloading (fake);
            } });

        steps.push_back ({ "Audio: host deletes the voice channel -> it and its clip are gone everywhere",
            [this] { song::removeChannel (H(), *ch (H(), "voice")); },
            [this]
            {
                return hashesEqual() && compiledEqualEverywhere()
                    && everywhere ([this] (te::Edit& e) { return ch (e, "voice") == nullptr && ! audioClipOf (e, voiceHash).isValid(); });
            },
            8000, 600 });

        //==============================================================================
        // Recording (a simulated sound card with one mic input; peer 2 records)
        steps.push_back ({ "Recording: simulated sound card; peer 2 makes insert 'Vocals', picks the mic input and arms it (stays on peer 2)",
            [this]
            {
                soundCard = std::make_unique<SimulatedSoundCard> (engine);
                auto* vocals = song::addInsert (P2(), "Vocals");
                vocalsID = vocals->itemID.toString();
                auto inputs = peer2.getRecorder().getInputs();
                std::cout << "    inputs on this PC: " << inputs.size() << (inputs.isEmpty() ? juce::String() : " (" + inputs[0].name + ")")
                          << std::endl;

                if (! inputs.isEmpty())
                    peer2.getRecorder().setInputFor (vocalsID, inputs[0].id);
            },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return insert (e, "Vocals") != nullptr; })
                    && peer2.getRecorder().getNumArmed() == 1
                    && host.getRecorder().getNumArmed() == 0 && host.getRecorder().getInputFor (vocalsID).isEmpty()
                    && peer1.getRecorder().getNumArmed() == 0
                    && hashesEqual();   // choosing an input changed nothing in the shared song
            },
            8000, 600 });

        steps.push_back ({ "Recording: peer 2 presses Rec at beat 8 -> recording on peer 2 only (not sharing); everyone reads it in the chat",
            [this] { recordStarted = peer2.startRecording (P2().tempoSequence.toTime (te::BeatPosition::fromBeats (8.0)).inSeconds()); },
            [this]
            {
                return recordStarted && peer2.isRecording() && peer2.isSongMode()
                    && ! H().getTransport().isPlaying() && ! P1().getTransport().isPlaying()
                    && chatCount (host, "peer2 started recording") == 1 && chatCount (peer1, "peer2 started recording") == 1;
            },
            8000 });

        steps.push_back ({ "Recording: 1 s of loud tone goes in; meanwhile host and peer 1 edit -> arrives at peer 2, still recording",
            [this]
            {
                const juce::int64 fedBefore = soundCard->samplesFed;

                soundCard->feed (1.0, 0.5f);

                // Where the song really was after that second: from that, which input sample
                // was going in while the song played the take's start position (beat 8).
                if (auto* ctx = P2().getTransport().getCurrentPlaybackContext())
                {
                    const double played = ctx->getPosition().inSeconds()
                                           - P2().tempoSequence.toTime (te::BeatPosition::fromBeats (8.0)).inSeconds();
                    fedAtTakeStart = soundCard->samplesFed - (juce::int64) std::llround (played * SimulatedSoundCard::sampleRate);
                    std::cout << "    playhead moved " << played << " s while " << (soundCard->samplesFed - fedBefore)
                              << " samples went in -> the song's beat 8 met input sample " << fedAtTakeStart << std::endl;
                }

                song::setStep (H(), pat (H(), "Pattern 1"), *ch (H(), "Hat"), 11, true);
                song::setStep (P1(), pat (P1(), "Pattern 1"), *ch (P1(), "Clap"), 13, true);
            },
            [this]
            {
                return hasStep (P2(), "Pattern 1", "Hat", 11) && hasStep (P2(), "Pattern 1", "Clap", 13)
                    && peer2.isRecording() && hashesEqual();
            },
            8000, 600 });

        steps.push_back ({ "Recording: 1 s of quieter tone, peer 2 stops -> 'Vocals take 1' routed to 'Vocals', clip at beat 8, on every PC",
            [this]
            {
                soundCard->feed (1.0, 0.25f);
                peer2.setPlaying (false, P2().tempoSequence.toTime (te::BeatPosition::fromBeats (8.0)).inSeconds());
            },
            [this]
            {
                soundCard->feed (0.01, 0.0f);   // keep the (stopped) engine ticking

                auto clip = clipOfChannel (P2(), "Vocals take 1");
                takeHash = clip[song::ids::asset].toString();

                if (! clip.isValid())
                    return false;

                const bool ok = everywhere ([this] (te::Edit& e)
                {
                    auto* take = ch (e, "Vocals take 1");
                    auto c = clipOfChannel (e, "Vocals take 1");
                    return take != nullptr && song::isAudioChannel (*take) && c.isValid()
                        && song::resolveRoute (e, *take) == insert (e, "Vocals")
                        && c[song::ids::asset].toString() == takeHash
                        && std::abs ((double) c[song::ids::start] - 8.0) < 0.01
                        && std::abs ((double) c[song::ids::duration] - 2.0) < 0.03;
                })
                && hasVerified (host, takeHash) && hasVerified (peer1, takeHash)
                && ! peer2.isRecording() && ! H().getTransport().isPlaying()
                && hashesEqual() && compiledEqualEverywhere();

                if (ok)
                    std::cout << "    take: start beat " << (double) clip[song::ids::start] << ", "
                              << (double) clip[song::ids::duration] << " s, offset " << (double) clip[song::ids::offset] << " s" << std::endl;

                return ok;
            },
            20000 });

        steps.push_back ({ "Recording: the take holds exactly what went in and lines up to the sample with what was playing - host's copy",
            {},
            [this]
            {
                auto file = host.getAssetStore().getFile (takeHash);
                double length = 0.0;
                const float start = peakOf (file, 0.0, 0.02, &length);
                const float first = peakOf (file, 0.1, 0.9);
                const float second = peakOf (file, 1.1, 1.9);
                std::cout << "    length " << length << " s, peak in first 20 ms " << start << ", first second "
                          << first << ", second second " << second << std::endl;

                // Alignment: which input sample is the take's first one? (match the sine's phase)
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
                juce::int64 firstSample = -1;

                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> b (1, 256);
                    reader->read (&b, 0, 256, 0, true, false);
                    double bestError = 1.0e9;

                    for (int k = 0; k < 4096; ++k)
                    {
                        double err = 0.0;

                        for (int j = 0; j < 256; ++j)
                            err += std::abs (b.getSample (0, j) - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 437.31
                                                                                   * (double) (k + j) / SimulatedSoundCard::sampleRate));

                        if (err < bestError)
                        {
                            bestError = err;
                            firstSample = k;
                        }
                    }
                }

                auto clip = clipOfChannel (H(), "Vocals take 1");
                const auto expected = fedAtTakeStart + (juce::int64) std::llround ((double) clip[song::ids::offset] * SimulatedSoundCard::sampleRate);
                std::cout << "    the take starts with input sample " << firstSample << "; the song was at beat 8 at input sample "
                          << expected << " -> " << (firstSample == expected ? "lined up exactly" : "OFF") << std::endl;

                return std::abs (length - 2.0) < 0.03 && start > 0.4f
                    && std::abs (first - 0.5f) < 0.03f && std::abs (second - 0.25f) < 0.03f
                    && firstSample == expected;
            } });

        steps.push_back ({ "Recording with a 1-bar count-in: the interface runs on its own; peer 2 presses Rec at beat 16 -> the count-in plays first",
            [this]
            {
                peer2.setMetronome (false);
                peer2.setCountInBars (1);
                soundCard->startFeeding (0.5f, P2());   // like a real interface: blocks keep coming
                juce::Thread::sleep (100);
                recordStarted = peer2.startRecording (timeOfBeat (P2(), 16.0));
            },
            [this]
            {
                return recordStarted && peer2.isRecording() && soundCard->lastPosition() >= 0.0
                    && soundCard->lastPosition() < timeOfBeat (P2(), 16.0 - 3.9);
            },
            8000 });

        steps.push_back ({ "Count-in: after the count-in and 1 s of singing, peer 2 stops -> 'Vocals take 2' starts exactly at beat 16",
            {},
            [this]
            {
                if (fedAtTake2Start < 0)
                {
                    if (soundCard->lastPosition() < timeOfBeat (P2(), 16.0) + 1.0)
                        return false;   // still counting in / singing

                    fedAtTake2Start = soundCard->inputSampleAt (timeOfBeat (P2(), 16.0));
                    peer2.setPlaying (false, timeOfBeat (P2(), 16.0));
                    return false;
                }

                auto clip = clipOfChannel (P2(), "Vocals take 2");

                if (clip.isValid())
                    soundCard->stopFeeding();

                return clip.isValid() && std::abs ((double) clip[song::ids::start] - 16.0) < 0.01
                    && (double) clip[song::ids::duration] > 0.9
                    && hasVerified (host, clip[song::ids::asset].toString()) && hashesEqual();
            },
            30000 });

        steps.push_back ({ "Count-in: take 2 lines up to the sample with what was playing (host's copy)",
            {},
            [this]
            {
                auto clip = clipOfChannel (H(), "Vocals take 2");
                auto file = host.getAssetStore().getFile (clip[song::ids::asset].toString());
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
                const auto expected = fedAtTake2Start + (juce::int64) std::llround ((double) clip[song::ids::offset] * SimulatedSoundCard::sampleRate);
                juce::int64 firstSample = -1;

                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> b (1, 256);
                    reader->read (&b, 0, 256, 0, true, false);
                    double bestError = 1.0e9;

                    for (juce::int64 k = expected - 4096; k < expected + 4096; ++k)
                    {
                        double err = 0.0;

                        for (int j = 0; j < 256; ++j)
                            err += std::abs (b.getSample (0, j) - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 437.31
                                                                                   * (double) (k + j) / SimulatedSoundCard::sampleRate));

                        if (err < bestError)
                        {
                            bestError = err;
                            firstSample = k;
                        }
                    }
                }

                std::cout << "    with count-in: the take starts with input sample " << firstSample << "; the song was at beat 16 at input sample "
                          << expected << " -> " << (firstSample == expected ? "lined up exactly" : "OFF") << std::endl;
                peer2.setCountInBars (0);
                return firstSample == expected;
            } });

        steps.push_back ({ "Host deletes the Snare channel -> gone everywhere, its notes too",
            [this]
            {
                auto* snare = ch (H(), "Snare");
                deletedChannelID = song::getChannelID (*snare);
                H().getUndoManager().beginNewTransaction();
                song::setStep (H(), pat (H(), "Pattern 1"), *snare, 4, true);
                H().getUndoManager().beginNewTransaction();
                song::removeChannel (H(), *snare);
            },
            [this]
            {
                return everywhere ([this] (te::Edit& e)
                {
                    return ch (e, "Snare") == nullptr && song::getNotes (pat (e, "Pattern 1"), deletedChannelID).isEmpty();
                }) && hashesEqual();
            },
            8000, 1600 });

        steps.push_back ({ "Host presses Undo -> the Snare channel and its step come back everywhere",
            [this] { H().undo(); },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return hasStep (e, "Pattern 1", "Snare", 4); }) && hashesEqual();
            },
            8000, 1600 });

        steps.push_back ({ "Peer 1 leaves (and loses its audio files); host keeps editing with peer 2",
            [this]
            {
                peer1.leave();
                peer1.setAssetFolder (testRoot.getChildFile ("peer1-new-pc-assets"));   // like a different PC
                song::setStep (H(), pat (H(), "Pattern 1"), *ch (H(), "Hat"), 15, true);
            },
            [this] { return hasStep (P2(), "Pattern 1", "Hat", 15) && host.getNumConnectedPeers() == 1; },
            8000, 1600 });

        steps.push_back ({ "People: host and peer 2 see that peer 1 left (list and chat)",
            {},
            [this]
            {
                return peopleNames (host).size() == 2 && peopleNames (peer2).size() == 2
                    && ! peopleNames (peer2).contains ("peer1")
                    && chatCount (host, "peer1 left") == 1 && chatCount (peer2, "peer1 left") == 1;
            } });

        steps.push_back ({ "Meanwhile the host shares and plays the song -> peer 2 follows",
            [this] { shareError = host.startSharing(); host.setPlaying (true, 0.0); },
            [this] { return shareError.isEmpty() && host.isSharing() && peer2.isFollowingShare() && P2().getTransport().isPlaying(); } });

        steps.push_back ({ "Peer 1 rejoins and gets the latest song -> it follows the running share right away",
            [this] { peer1.join ("127.0.0.1", port); },
            [this]
            {
                return allJoined() && hashesEqual() && hasStep (P1(), "Pattern 1", "Hat", 15)
                    && peer1.isFollowingShare() && P1().getTransport().isPlaying() && peer1.isSongMode();
            } });

        steps.push_back ({ "People: peer 1 is back in everyone's list and got the earlier chat",
            {},
            [this]
            {
                return peopleNames (host).size() == 3 && peopleNames (peer2).contains ("peer1")
                    && chatCount (peer1, "hello from peer 2") == 1 && chatCount (peer2, "peer1 joined") >= 2;
            } });

        steps.push_back ({ "Audio: the rejoined peer (no files) receives every audio file in the song",
            {},
            [this]
            {
                for (auto& h : song::getAssetHashes (P1()))
                    if (! hasVerified (peer1, h))
                        return false;

                return hasVerified (peer1, toneHash) && hasVerified (peer1, bigHash) && compiledEqualEverywhere();
            },
            40000 });

        steps.push_back ({ "The host stops and ends its share -> nobody plays or follows any more",
            [this] { host.setPlaying (false, 0.0); host.stopSharing(); },
            [this]
            {
                return ! host.isShareRunning() && ! peer1.isShareRunning() && ! peer2.isShareRunning()
                    && ! peer1.isFollowingShare() && ! peer2.isFollowingShare()
                    && ! H().getTransport().isPlaying() && ! P1().getTransport().isPlaying() && ! P2().getTransport().isPlaying();
            } });

        //==============================================================================
        // Browser: split by channel, deleting audio from the project
        steps.push_back ({ "Browser: host makes pattern 'Beat' (Kick + Hat) and places it twice (bar 11, lane 7 and bar 13, lane 9)",
            [this]
            {
                for (auto* sess : { &host, &peer1, &peer2 })
                    sess->setSongMode (true);

                auto beat = song::addPattern (H(), "Beat", 1);

                for (int step : { 0, 4, 8, 12 })
                    song::setStep (H(), beat, *ch (H(), "Kick"), step, true);

                for (int step : { 2, 6, 10, 14 })
                    song::setStep (H(), beat, *ch (H(), "Hat"), step, true);

                song::addClip (H(), beat[song::ids::sid].toString(), 6, 40.0);
                auto second = song::addClip (H(), beat[song::ids::sid].toString(), 8, 48.0);
                second.setProperty (song::ids::length, 2.0, &H().getUndoManager());   // a shortened placement
            },
            [this]
            {
                return everywhere ([] (te::Edit& e) { return pat (e, "Beat").isValid(); })
                    && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "Browser: peer 1 splits 'Beat' by channel -> 'Beat - Kick' + 'Beat - Hat' everywhere, the song plays the same notes",
            [this]
            {
                if (auto* c = host.getCompiler())
                    c->compileNow();

                compiledBeforeSplit = allCompiled (host);
                P1().getUndoManager().beginNewTransaction();
                auto parts = song::splitPatternByChannel (P1(), pat (P1(), "Beat"));
                std::cout << "    split into " << parts.size() << " patterns" << std::endl;
            },
            [this]
            {
                const bool structure = everywhere ([] (te::Edit& e)
                {
                    auto kick = pat (e, "Beat - Kick"), hat = pat (e, "Beat - Hat");
                    int kickClips = 0, hatClips = 0;

                    for (auto& c : song::getClips (e))
                    {
                        if (c[song::ids::pattern].toString() == kick[song::ids::sid].toString())  ++kickClips;
                        if (c[song::ids::pattern].toString() == hat[song::ids::sid].toString())   ++hatClips;
                    }

                    // Each placement: Kick and Hat parts at the same time, on different tracks.
                    bool separateTracks = true;

                    for (auto& k : song::getClips (e))
                    {
                        if (k[song::ids::pattern].toString() != kick[song::ids::sid].toString())
                            continue;

                        bool found = false;

                        for (auto& h : song::getClips (e))
                            if (h[song::ids::pattern].toString() == hat[song::ids::sid].toString()
                                 && (double) h[song::ids::start] == (double) k[song::ids::start]
                                 && (int) h[song::ids::lane] != (int) k[song::ids::lane])
                                found = true;

                        separateTracks = separateTracks && found;
                    }

                    return ! pat (e, "Beat").isValid() && kick.isValid() && hat.isValid()
                        && song::getNotes (kick, song::getChannelID (*ch (e, "Kick"))).size() == 4
                        && song::getNotes (hat, song::getChannelID (*ch (e, "Hat"))).size() == 4
                        && kickClips == 2 && hatClips == 2 && separateTracks;
                });

                if (! structure || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (auto* c = host.getCompiler())
                    c->compileNow();

                const auto after = allCompiled (host);
                std::cout << "    notes the song plays: " << compiledBeforeSplit.size() << " before, " << after.size()
                          << " after, identical: " << (after == compiledBeforeSplit ? "yes" : "NO") << std::endl;
                return after == compiledBeforeSplit && ! compiledBeforeSplit.isEmpty();
            },
            8000, 600 });

        steps.push_back ({ "Browser: peer 2 drags the unused 'voice' file back in -> a new 'voice' channel and clip everywhere",
            [this]
            {
                if (auto* channel = song::findOrCreateChannelForAsset (P2(), voiceHash))
                    song::placeAudioChannel (P2(), *channel, 10, 60.0);
            },
            [this]
            {
                return everywhere ([this] (te::Edit& e) { return ch (e, "voice") != nullptr && audioClipOf (e, voiceHash).isValid(); })
                    && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "Browser: host deletes 'voice' and 'tone' from the project -> clips, channels and files gone everywhere",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                song::removeAsset (H(), voiceHash);
                song::removeAsset (H(), toneHash);
            },
            [this]
            {
                return everywhere ([this] (te::Edit& e)
                {
                    const auto hashes = song::getAssetHashes (e);
                    return ch (e, "voice") == nullptr && ch (e, "tone") == nullptr
                        && ! audioClipOf (e, voiceHash).isValid() && ! audioClipOf (e, toneHash).isValid()
                        && ! hashes.contains (voiceHash) && ! hashes.contains (toneHash)
                        && hashes.contains (bigHash);
                })
                && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        //==============================================================================
        // Song files
        steps.push_back ({ "Song file: host names the song and saves it -> a project folder: the song, Audio\\ and Takes\\ with readable names",
            [this]
            {
                song::setTitle (H(), "test song");
                projectFile = appdata::projectSongFile (testRoot.getChildFile ("Projects").getChildFile ("test song.cotosong"));
                host.saveProject (projectFile, true, [this] (project::SaveResult r) { saveResult = r; saveDone = true; });
            },
            [this]
            {
                if (! saveDone)
                    return false;

                const auto folder = projectFile.getParentDirectory();
                juce::StringArray audioFiles, takeFiles;

                for (auto& f : folder.getChildFile ("Audio").findChildFiles (juce::File::findFiles, false))  audioFiles.add (f.getFileName());
                for (auto& f : folder.getChildFile ("Takes").findChildFiles (juce::File::findFiles, false))  takeFiles.add (f.getFileName());

                audioFiles.sort (true);
                takeFiles.sort (true);
                std::cout << "    " << (saveResult.ok() ? "saved " + juce::File::descriptionOfSizeInBytes (saveResult.bytes)
                                                          + " in " + folder.getFileName() + ", " + juce::String (saveResult.audioIncluded) + " audio files"
                                                        : "error: " + saveResult.error) << std::endl;
                std::cout << "    Audio: " << audioFiles.joinIntoString (", ") << "  |  Takes: " << takeFiles.joinIntoString (", ") << std::endl;

                return saveResult.ok() && projectFile.existsAsFile() && folder.getFileName() == "test song"
                    && saveResult.audioMissing == 0
                    && saveResult.audioIncluded == song::getAssetHashes (H()).size()
                    && audioFiles.size() + takeFiles.size() == saveResult.audioIncluded
                    && takeFiles.contains ("Vocals take 1.flac") && audioFiles.contains ("big.flac")
                    && hashesEqual();
            },
            30000, 600 });

        steps.push_back ({ "Song file: saving again copies no audio twice",
            [this]
            {
                saveDone = false;
                host.saveProject (projectFile, true, [this] (project::SaveResult r) { saveResult = r; saveDone = true; });
            },
            [this]
            {
                if (! saveDone)
                    return false;

                const int files = projectFile.getParentDirectory().getChildFile ("Audio").getNumberOfChildFiles (juce::File::findFiles)
                                + projectFile.getParentDirectory().getChildFile ("Takes").getNumberOfChildFiles (juce::File::findFiles);
                std::cout << "    new audio files written: " << saveResult.audioWritten << ", files in the folder: " << files << std::endl;
                return saveResult.ok() && saveResult.audioWritten == 0 && files == saveResult.audioIncluded;
            },
            30000 });

        steps.push_back ({ "Song file: a 4th PC (alone, empty audio folder) opens it -> identical song, audio restored and checked, same notes",
            [this]
            {
                fourth.peerName = "fourth";
                fourth.setAssetFolder (testRoot.getChildFile ("fourth-assets"));
                fourth.openProject (projectFile, [this] (juce::String error) { openError = error; openDone = true; });
            },
            [this]
            {
                if (! openDone || fourth.getEdit() == nullptr)
                    return false;

                if (openError.isNotEmpty())
                {
                    std::cout << "    error: " << openError << std::endl;
                    return false;
                }

                for (auto& h : song::getAssetHashes (*fourth.getEdit()))
                    if (! hasVerified (fourth, h))
                        return false;

                fourth.setSongMode (host.isSongMode());

                if (auto* c = fourth.getCompiler())     c->compileNow();
                if (auto* c = host.getCompiler())       c->compileNow();

                const bool sameSong = fourth.getSync()->computeHash() == host.getSync()->computeHash();
                const bool sameNotes = allCompiled (fourth) == allCompiled (host);
                std::cout << "    same song: " << (sameSong ? "yes" : "NO") << ", same notes: " << (sameNotes ? "yes" : "NO")
                          << ", title: " << song::getTitle (*fourth.getEdit()) << std::endl;
                return sameSong && sameNotes && song::getTitle (*fourth.getEdit()) == "test song";
            },
            30000 });

        steps.push_back ({ "Song file: an older .syncsong (audio inside the file) still opens on a PC without the audio",
            [this]
            {
                // Built the way older versions saved: song.xml + audio/<fingerprint>.flac in one zip.
                legacyFile = testRoot.getChildFile ("legacy").getChildFile ("test song.syncsong");   // (same name: same song title)
                legacyFile.getParentDirectory().createDirectory();
                juce::ZipFile::Builder zip;
                juce::XmlElement info ("SYNCSONG");
                info.setAttribute ("format", 1);
                const auto infoText = info.toString(), songText = host.getSync()->createSnapshot().toXmlString();
                zip.addEntry (new juce::MemoryInputStream (infoText.toRawUTF8(), infoText.getNumBytesAsUTF8(), true), 9, "info.xml", {});
                zip.addEntry (new juce::MemoryInputStream (songText.toRawUTF8(), songText.getNumBytesAsUTF8(), true), 9, "song.xml", {});

                for (auto& h : song::getAssetHashes (H()))
                    zip.addFile (host.getAssetStore().getFile (h), 0, "audio/" + h + ".flac");

                juce::FileOutputStream out (legacyFile);
                zip.writeToStream (out, nullptr);
                out.flush();

                openDone = false;
                openError = "not done";
                fourth.setAssetFolder (testRoot.getChildFile ("fourth-assets-2"));   // empty again
                fourth.openProject (legacyFile, [this] (juce::String error) { openError = error; openDone = true; });
            },
            [this]
            {
                if (! openDone || fourth.getEdit() == nullptr)
                    return false;

                for (auto& h : song::getAssetHashes (*fourth.getEdit()))
                    if (! hasVerified (fourth, h))
                        return false;

                return openError.isEmpty() && fourth.getSync()->computeHash() == host.getSync()->computeHash();
            },
            30000 });

        steps.push_back ({ "Data folder: the files of the old version move off C: into the new folder (settings, audio, backups, songs, exports)",
            [this]
            {
                // An old PC: %APPDATA%\\SyncTest and Documents\\SyncTest Songs.
                const auto old = testRoot.getChildFile ("old-pc");
                const auto appData = old.getChildFile ("AppData").getChildFile ("SyncTest");
                const auto songs = old.getChildFile ("Documents").getChildFile ("SyncTest Songs");
                const auto hash = song::getAssetHashes (H())[0];
                appData.getChildFile ("Assets").createDirectory();
                host.getAssetStore().getFile (hash).copyFileTo (appData.getChildFile ("Assets").getChildFile (hash + ".flac"));
                appData.getChildFile ("Assets-recordings").getChildFile ("take1.wav").create();
                appData.getChildFile ("Temporary").getChildFile ("x.tmp").create();
                appData.getChildFile ("Autosave").createDirectory();
                appData.getChildFile ("Autosave").getChildFile ("a (backup).syncsong").replaceWithText ("backup");
                appData.getChildFile ("Settings.xml").replaceWithText ("<PROPERTIES/>");
                songs.createDirectory();
                legacyFile.copyFileTo (songs.getChildFile ("My Song.syncsong"));
                songs.getChildFile ("Exports").getChildFile ("My Song - stems").createDirectory();
                songs.getChildFile ("Exports").getChildFile ("My Song.wav").replaceWithText ("wav");
                songs.getChildFile ("Exports").getChildFile ("My Song - stems").getChildFile ("01 Drums.wav").replaceWithText ("wav");

                {
                    juce::PropertiesFile oldSettings (appData.getChildFile ("SyncTest.settings"), juce::PropertiesFile::Options());
                    oldSettings.setValue ("userName", "Martin");
                    oldSettings.setValue ("recentSongs", songs.getChildFile ("My Song.syncsong").getFullPathName());
                    oldSettings.saveIfNeeded();
                }

                const auto root = testRoot.getChildFile ("new-data-folder");
                const auto plan = appdata::planMigration (appData, songs, root);
                migrationResult = appdata::runMigration (plan, {});
                migrationHash = hash;
            },
            [this]
            {
                const auto old = testRoot.getChildFile ("old-pc");
                const auto root = testRoot.getChildFile ("new-data-folder");
                const auto song = root.getChildFile ("Projects").getChildFile ("My Song").getChildFile ("My Song.syncsong");
                juce::PropertiesFile settings (root.getChildFile ("Settings").getChildFile ("COTOSTUDIO.settings"), juce::PropertiesFile::Options());

                std::cout << "    moved " << migrationResult.filesMoved << " files (" << migrationResult.bytesMoved << " bytes), "
                          << migrationResult.failures << " failures; recent songs: " << settings.getValue ("recentSongs") << std::endl;

                return migrationResult.failures == 0
                    && song.existsAsFile()
                    && settings.getValue ("userName") == "Martin" && settings.getValue ("recentSongs") == song.getFullPathName()
                    && root.getChildFile ("Settings").getChildFile ("Settings.xml").existsAsFile()
                    && AssetStore::fingerprint (root.getChildFile ("Cache").getChildFile ("Audio").getChildFile (migrationHash + ".flac")) == migrationHash
                    && root.getChildFile ("Backups").getChildFile ("a (backup).syncsong").existsAsFile()
                    && root.getChildFile ("Exports").getChildFile ("My Song.wav").existsAsFile()
                    && root.getChildFile ("Exports").getChildFile ("My Song - stems").getChildFile ("01 Drums.wav").existsAsFile()
                    && ! old.getChildFile ("AppData").getChildFile ("SyncTest").exists()          // nothing left on "C:"
                    && ! old.getChildFile ("Documents").getChildFile ("SyncTest Songs").exists();
            },
            4000 });

        steps.push_back ({ "Song file: host starts a new song with both peers connected -> everyone gets it (no resync)",
            [this]
            {
                resyncsBefore = peer1.getResyncCount() + peer2.getResyncCount();
                newError = host.newProject();
            },
            [this]
            {
                return newError.isEmpty()
                    && everywhere ([] (te::Edit& e)
                       {
                           return song::getChannels (e).size() == 5 && song::getPatterns (e).size() == 1
                               && song::getClips (e).size() == 1 && song::getTitle (e) == "Untitled";
                       })
                    && hashesEqual() && peer1.getResyncCount() + peer2.getResyncCount() == resyncsBefore
                    && everywhere ([] (te::Edit& e) { return idsUnique (e); });
            },
            10000, 600 });

        steps.push_back ({ "Song file: host opens the saved song again -> both peers get it back with its audio, IDs stay unique",
            [this]
            {
                openDone = false;
                openError = "not done";
                host.openProject (projectFile, [this] (juce::String error) { openError = error; openDone = true; });
            },
            [this]
            {
                if (! openDone || openError.isNotEmpty())
                    return false;

                for (auto* s : { &peer1, &peer2 })
                    for (auto& h : song::getAssetHashes (*s->getEdit()))
                        if (! hasVerified (*s, h))
                            return false;

                return hashesEqual() && compiledEqualEverywhere()
                    && everywhere ([] (te::Edit& e)
                       {
                           return pat (e, "Beat - Kick").isValid() && song::getTitle (e) == "test song" && idsUnique (e);
                       })
                    && peer1.getResyncCount() + peer2.getResyncCount() == resyncsBefore;
            },
            30000, 600 });

        steps.push_back ({ "Song file: afterwards peer 1 adds a channel and peer 2 a note -> everywhere, no ID clashes",
            [this]
            {
                channelsBefore = song::getChannels (H()).size();
                song::addChannel (P1(), song::InstrumentKind::synth);
                song::setStep (P2(), pat (P2(), "Beat - Kick"), *ch (P2(), "Kick"), 6, true);
            },
            [this]
            {
                return hashesEqual() && compiledEqualEverywhere()
                    && everywhere ([this] (te::Edit& e)
                       {
                           return song::getChannels (e).size() == channelsBefore + 1 && idsUnique (e)
                               && hasStep (e, "Beat - Kick", "Kick", 6);
                       });
            },
            8000, 600 });

        //==============================================================================
        // Export (on a private copy of the song, in the background)
        steps.push_back ({ "Export: the host exports the song as 24-bit WAV + stems -> the mix and one file per insert; the shared song is untouched",
            [this]
            {
                resyncsBefore = peer1.getResyncCount() + peer2.getResyncCount();
                startExport (testRoot.getChildFile ("export").getChildFile ("mix.wav"), true, Exporter::Format::wav24, true);
            },
            [this]
            {
                if (! exportDone)
                    return false;

                if (! exportResult.ok())
                {
                    std::cout << "    error: " << exportResult.error << std::endl;
                    return false;
                }

                auto mix = testRoot.getChildFile ("export").getChildFile ("mix.wav");
                auto stems = testRoot.getChildFile ("export").getChildFile ("mix - stems");
                const double songSeconds = timeOfBeat (H(), song::getSongLengthBeats (H()));
                double length = 0.0;
                const float peak = peakOf (mix, 0.0, juce::jmin (songSeconds, 20.0), &length);
                const int numStems = stems.getNumberOfChildFiles (juce::File::findFiles, "*.wav");
                const float drumsPeak = peakOf (stems.getChildFile ("01 Drums.wav"), 0.0, juce::jmin (songSeconds, 20.0));

                std::cout << "    mix: " << length << " s (song " << songSeconds << " s), peak " << peak << "; "
                          << numStems << " stems, Drums stem peak " << drumsPeak << std::endl;

                return length >= songSeconds - 0.01 && length < songSeconds + 10.5 && peak > 0.05f
                    && numStems == 1 + song::getInserts (H()).size() && drumsPeak > 0.01f
                    && exportResult.files.size() == numStems + 1
                    && hashesEqual() && peer1.getResyncCount() + peer2.getResyncCount() == resyncsBefore;
            },
            60000 });

        steps.push_back ({ "Export: Pattern 1 as FLAC -> a seamless loop exactly one pattern long",
            [this] { startExport (testRoot.getChildFile ("export-pattern").getChildFile ("loop.flac"), false, Exporter::Format::flac, false); },
            [this]
            {
                if (! exportDone)
                    return false;

                auto file = testRoot.getChildFile ("export-pattern").getChildFile ("loop.flac");
                const double expected = timeOfBeat (H(), song::getBars (pat (H(), "Pattern 1")) * 4.0);
                const double length = lengthOf (file);
                std::cout << "    loop: " << length << " s, pattern " << expected << " s" << std::endl;
                return exportResult.ok() && std::abs (length - expected) < 0.001 && peakOf (file, 0.0, length) > 0.05f;
            },
            60000 });

        steps.push_back ({ "Export: MP3 320 kbps (Windows' encoder) -> plays back at the right length",
            [this] { startExport (testRoot.getChildFile ("export-mp3").getChildFile ("song.mp3"), true, Exporter::Format::mp3_320, false); },
            [this]
            {
                if (! exportDone)
                    return false;

                if (! exportResult.ok())
                {
                    std::cout << "    error: " << exportResult.error << std::endl;
                    return false;
                }

                auto file = testRoot.getChildFile ("export-mp3").getChildFile ("song.mp3");
                const double songSeconds = timeOfBeat (H(), song::getSongLengthBeats (H()));
                double length = 0.0;
                const float peak = peakOf (file, 0.0, juce::jmin (songSeconds, 20.0), &length);
                std::cout << "    mp3: " << juce::File::descriptionOfSizeInBytes (file.getSize()) << ", " << length << " s, peak " << peak << std::endl;
                return length >= songSeconds - 0.05 && peak > 0.05f;
            },
            90000, 0, ! Exporter::canExportMp3() });

        //==============================================================================
        // Your sounds (the sound browser): previewing is only yours; a sound or MIDI file
        // dropped into the song is shared like everything else.
        steps.push_back ({ "Sounds: clicking a sound in your folders plays it to your speakers only - the song doesn't change",
            [this]
            {
                beatsFolder = testRoot.getChildFile ("My Beats");
                boomFile = writeHitWav (beatsFolder.getChildFile ("Kit 1").getChildFile ("Boom 01.wav"), 0.3, 70.0);
                longFile = writeHitWav (beatsFolder.getChildFile ("Kit 1").getChildFile ("808 long.wav"), 1.5, 50.0);
                midiFile = writeTestMidi (beatsFolder.getChildFile ("MIDI").getChildFile ("melody.mid"));
                contentBeforePreview = host.getSync()->computeContentHash();
                assetsBeforePreview = song::getAssets (H()).size();
                previewer = std::make_unique<SamplePreviewer> (engine);
                previewer->play (longFile);
            },
            [this]
            {
                const float playing = soundCard->feedSilenceAndMeasure (0.3);

                if (playing < 0.05f)
                {
                    previewer->play (longFile);   // the read-ahead wasn't ready yet: try again
                    return false;
                }

                previewer->stop();
                soundCard->feedSilenceAndMeasure (0.05);   // (the stop fades out within a block)
                const float stopped = soundCard->feedSilenceAndMeasure (0.2);
                std::cout << "    speakers while previewing: peak " << playing << "; after Stop: " << stopped << std::endl;

                return stopped < 0.001f && host.getSync()->computeContentHash() == contentBeforePreview
                    && song::getAssets (H()).size() == assetsBeforePreview && hashesEqual();
            },
            8000 });

        steps.push_back ({ "Sounds: peer 1 drops 'Boom 01.wav' on the channel rack -> a new sampler channel everywhere; the file reaches every PC and each loads it",
            [this]
            {
                auto r = peer1.getAssetStore().importAudioFile (boomFile);
                boomHash = r.hash;
                P1().getUndoManager().beginNewTransaction();
                song::addSamplerChannel (P1(), r, boomFile.getFileName());
            },
            [this]
            {
                return everywhere ([this] (te::Edit& e)
                {
                    auto* t = ch (e, "Boom 01");
                    return t != nullptr && song::isSamplerChannel (*t) && song::getSamplerAsset (*t) == boomHash
                        && PatternCompiler::describeLoadedSample (*t) == boomHash;
                })
                && hasVerified (host, boomHash) && hasVerified (peer2, boomHash) && hashesEqual();
            },
            10000, 600 });

        steps.push_back ({ "Sounds: peer 2 puts 'Boom 01' on steps 1 and 9 of Pattern 1 -> everywhere; it sounds exactly on those steps (render)",
            [this]
            {
                auto p = pat (P2(), "Pattern 1");

                if (auto* t = ch (P2(), "Boom 01"))
                {
                    P2().getUndoManager().beginNewTransaction();
                    song::setStep (P2(), p, *t, 0, true);
                    song::setStep (P2(), p, *t, 8, true);
                }

                p2WasSongMode = peer2.isSongMode();
                peer2.setSongMode (false);   // only peer 2's own playback
                peer2.setPlayingPattern (p[song::ids::sid].toString());
            },
            [this]
            {
                if (! everywhere ([] (te::Edit& e) { return hasStep (e, "Pattern 1", "Boom 01", 0) && hasStep (e, "Pattern 1", "Boom 01", 8); })
                     || ! hashesEqual())
                    return false;

                peer2.getCompiler()->compileNow();
                auto peaks = renderPeaks (P2(), { "Boom 01" }, { { 0.0, 0.4 }, { 1.2, 1.9 }, { 2.0, 2.4 }, { 3.2, 3.9 } });
                std::cout << "    peaks: step 1 " << peaks[0] << ", between " << peaks[1] << ", step 9 " << peaks[2]
                          << ", after " << peaks[3] << std::endl;
                return peaks[0] > 0.2f && peaks[2] > 0.2f && peaks[1] >= 0.0f && peaks[1] < 0.01f && peaks[3] < 0.01f;
            },
            15000, 600 });

        steps.push_back ({ "Sounds: the host drops '808 long.wav' on that channel -> it plays the new sound everywhere (same channel, same steps)",
            [this]
            {
                peer2.setSongMode (p2WasSongMode);
                auto r = host.getAssetStore().importAudioFile (longFile);
                longHash = r.hash;

                if (auto* t = ch (H(), "Boom 01"))
                {
                    H().getUndoManager().beginNewTransaction();
                    song::setSamplerSample (H(), *t, r, longFile.getFileName());
                }
            },
            [this]
            {
                return everywhere ([this] (te::Edit& e)
                {
                    auto* t = ch (e, "808 long");
                    return t != nullptr && ch (e, "Boom 01") == nullptr && song::getSamplerAsset (*t) == longHash
                        && PatternCompiler::describeLoadedSample (*t) == longHash
                        && hasStep (e, "Pattern 1", "808 long", 0) && hasStep (e, "Pattern 1", "808 long", 8);
                })
                && hasVerified (peer1, longHash) && hasVerified (peer2, longHash) && hashesEqual();
            },
            10000, 600 });

        steps.push_back ({ "Sounds: peer 1 turns on 'Cut itself' for the 808 -> everywhere; a copy made for exporting loads the sound too",
            [this]
            {
                if (auto* t = ch (P1(), "808 long"))
                    if (auto* sampler = dynamic_cast<SamplerPlugin*> (song::getInstrument (*t)))
                    {
                        P1().getUndoManager().beginNewTransaction();
                        sampler->cutValue = true;
                    }

                // What the exporter does: a private copy of the song, compiled on its own.
                auto copyState = host.getSync()->createSnapshot();
                copyState.setProperty (te::IDs::projectID, te::ProjectItemID::createNewID (te::ProjectID {}).toString(), nullptr);

                if (auto copy = te::loadEditFromState (engine, copyState, te::Edit::forRendering))
                {
                    PatternCompiler compiler (*copy, host.getAssetStore());
                    auto* t = ch (*copy, "808 long");
                    exportCopyLoaded = t != nullptr && PatternCompiler::describeLoadedSample (*t) == longHash;
                }
            },
            [this]
            {
                return exportCopyLoaded && hashesEqual() && everywhere ([] (te::Edit& e)
                {
                    auto* t = ch (e, "808 long");
                    auto* sampler = t != nullptr ? dynamic_cast<SamplerPlugin*> (song::getInstrument (*t)) : nullptr;
                    return sampler != nullptr && sampler->cutValue.get();
                });
            },
            8000, 600 });

        steps.push_back ({ "Sounds: peer 2 drops melody.mid on the Synth, the host drops it on empty rack space -> the notes replace Synth's, a new 'melody' channel; everywhere",
            [this]
            {
                if (auto* synth = ch (P2(), "Synth"))
                {
                    P2().getUndoManager().beginNewTransaction();
                    auto id = song::getChannelID (*synth);
                    midiLogOnSynth = fileimport::applyMidiFile (P2(), pat (P2(), "Pattern 1"), midiFile, id);
                }

                H().getUndoManager().beginNewTransaction();
                juce::String none;
                midiLogNew = fileimport::applyMidiFile (H(), pat (H(), "Pattern 1"), midiFile, none);
                std::cout << "    peer 2: " << midiLogOnSynth << "\n    host: " << midiLogNew << std::endl;
            },
            [this]
            {
                const juce::StringArray expected { "60@0.00+1.00v100", "64@1.00+1.00v90", "67@2.00+0.50v80", "72@4.00+2.00v110" };

                return midiLogOnSynth.contains ("left out") && hashesEqual()
                    && everywhere ([&expected] (te::Edit& e)
                       {
                           return notesIn (e, "Synth") == expected && notesIn (e, "melody") == expected
                               && song::getBars (pat (e, "Pattern 1")) >= 2;
                       });
            },
            8000, 600 });

        steps.push_back ({ "Sounds: the host deletes '808 long.wav' from the project -> its sampler channel goes too, everywhere",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                song::removeAsset (H(), longHash);
            },
            [this]
            {
                return everywhere ([this] (te::Edit& e) { return ch (e, "808 long") == nullptr && ! song::getAssetHashes (e).contains (longHash); })
                    && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "Same moment, same channel, different things: host renames Clap and turns its volume down while peer 2 mutes it and pans it -> all four kept everywhere",
            [this]
            {
                auto* hostClap = ch (H(), "Clap");
                auto* p2Clap = ch (P2(), "Clap");

                if (hostClap == nullptr || p2Clap == nullptr)
                    return;

                // Both before either change has been sent: they cross on the network.
                H().getUndoManager().beginNewTransaction();
                hostClap->setName ("Clap 2");
                song::getVolumePlugin (*hostClap)->volParam->setNormalisedParameter (0.5f, juce::sendNotification);

                P2().getUndoManager().beginNewTransaction();
                p2Clap->setMute (true);
                song::getVolumePlugin (*p2Clap)->panParam->setNormalisedParameter (0.2f, juce::sendNotification);
            },
            [this]
            {
                return hashesEqual() && everywhere ([] (te::Edit& e)
                {
                    auto* t = ch (e, "Clap 2");
                    auto* v = t != nullptr ? song::getVolumePlugin (*t) : nullptr;
                    return v != nullptr && t->isMuted (false)
                        && std::abs (v->volParam->getCurrentNormalisedValue() - 0.5f) < 0.001f
                        && std::abs (v->panParam->getCurrentNormalisedValue() - 0.2f) < 0.001f;
                });
            },
            8000, 600 });

        steps.push_back ({ "Final check: all copies identical, and so is what they play",
            [this] { dump ("host-final", H()); dump ("peer1-final", P1()); dump ("peer2-final", P2()); },
            [this] { return hashesEqual() && compiledEqualEverywhere() && engineRoutingMatchesEverywhere(); }, 4000, 1600 });
    }

    void timerCallback() override
    {
        if (current >= steps.size())
        {
            stopTimer();
            std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped << " skipped" << std::endl;

            for (auto& f : failures)
                std::cout << "  FAILED: " << f << std::endl;

            for (auto* sess : { &host, &peer1, &peer2 })
                if (auto* es = sess->getSync())
                {
                    auto st = es->getStats();
                    std::cout << sess->peerName << ": sent " << st.sent << ", applied " << st.applied
                              << ", failed " << st.failed << ", knobs sent " << st.paramsSent
                              << " / applied " << st.paramsApplied << "; top sent:" << std::endl;

                    for (auto& line : es->getTopSentKinds (6))
                        std::cout << "      " << line << std::endl;
                }

            std::cout << "resyncs: peer1 " << peer1.getResyncCount() << ", peer2 " << peer2.getResyncCount() << std::endl;
            finished (failed == 0 ? 0 : 1);
            return;
        }

        auto& step = steps[current];
        auto now = juce::Time::getMillisecondCounter();

        if (! actionRun)
        {
            std::cout << "\n> " << step.name << std::endl;

            if (step.optional && step.name.contains ("VST3") && ! pluginFile.exists())
            {
                std::cout << "  SKIPPED (no --plugin given)" << std::endl;
                ++skipped;
                advance();
                return;
            }

            actionRun = true;
            stepStart = now;

            if (step.action)
                step.action();

            return;
        }

        const auto elapsed = (int) (now - stepStart);

        if (elapsed >= step.minWaitMs && step.done())
        {
            std::cout << "  PASS (" << elapsed << " ms)" << std::endl;
            ++passed;
            advance();
        }
        else if (elapsed > step.timeoutMs)
        {
            if (step.optional)
            {
                std::cout << "  SKIPPED (optional; not supported in this environment)" << std::endl;
                ++skipped;
            }
            else
            {
                std::cout << "  FAIL (timed out after " << elapsed << " ms)" << std::endl;
                ++failed;
                failures.add (step.name);
            }

            advance();
        }
    }

    void advance()
    {
        ++current;
        actionRun = false;
    }
};

SelfTest::SelfTest (te::Engine& e, const juce::StringArray& args, std::function<void (int)> onFinished)
    : impl (std::make_unique<Impl> (e, args, std::move (onFinished)))
{
}

SelfTest::~SelfTest() = default;

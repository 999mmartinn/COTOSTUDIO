// Automatic test: one host and two joiners in the same process, talking over
// real TCP sockets on 127.0.0.1. Run with:
//   SyncTest --selftest [--plugin=<path to a VST3 effect>] [--dump=<dir>]

#include "SelfTest.h"
#include "Bouncer.h"
#include "Backups.h"
#include <iostream>
#include <thread>
#include <mutex>
#include "Session.h"
#include "SongModel.h"
#include "PatternCompiler.h"
#include "DrumSynthPlugin.h"
#include "ParametricEQPlugin.h"
#include "LimiterPlugin.h"
#include "DelayPlugin.h"
#include "ReverbPlugin.h"
#include "SidechainPlugin.h"
#include "MixerRouter.h"
#include "ProjectFile.h"
#include "Scales.h"
#include "Exporter.h"
#include "SamplerPlugin.h"
#include "EditorState.h"
#include "SamplePreviewer.h"
#include "FileImport.h"
#include "AppData.h"
#include "FlpImport.h"
#include "MoreFx.h"

namespace
{
    /** Writes a small FL Studio project (.flp) the way FL does, for the import test. */
    struct FlpWriter
    {
        juce::MemoryOutputStream events;

        void byteEvent (int id, int v)               { events.writeByte ((char) id); events.writeByte ((char) v); }
        void wordEvent (int id, int v)               { events.writeByte ((char) id); events.writeShort ((short) v); }
        void dwordEvent (int id, juce::uint32 v)     { events.writeByte ((char) id); events.writeInt ((int) v); }

        void dataEvent (int id, const void* data, size_t size)
        {
            events.writeByte ((char) id);

            for (auto n = size;;)
            {
                auto b = (juce::uint8) (n & 0x7f);
                n >>= 7;
                events.writeByte ((char) (n > 0 ? (b | 0x80) : b));
                if (n == 0) break;
            }

            events.write (data, size);
        }

        void dataEvent (int id, const juce::MemoryBlock& m)    { dataEvent (id, m.getData(), m.getSize()); }

        void textEvent (int id, const juce::String& s)          // UTF-16, as FL writes texts
        {
            juce::MemoryOutputStream t;
            for (auto p = s.getCharPointer(); ! p.isEmpty();)
                t.writeShort ((short) p.getAndAdvance());
            t.writeShort (0);
            dataEvent (id, t.getData(), t.getDataSize());
        }

        static void record (juce::MemoryOutputStream& o, int id, const juce::MemoryBlock& m)
        {
            o.writeInt (id);
            o.writeInt64 ((juce::int64) m.getSize());
            o.write (m.getData(), m.getSize());
        }

        static juce::MemoryBlock text8 (const juce::String& s)  { return juce::MemoryBlock (s.toRawUTF8(), s.getNumBytesAsUTF8()); }

        juce::MemoryBlock finish()
        {
            juce::MemoryOutputStream f;
            f.write ("FLhd", 4);  f.writeInt (6);  f.writeShort (0);  f.writeShort (1);  f.writeShort (96);
            f.write ("FLdt", 4);  f.writeInt ((int) events.getDataSize());
            f.write (events.getData(), events.getDataSize());
            return f.getMemoryBlock();
        }
    };

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
            if (a.startsWith ("--only="))     onlySteps = a.fromFirstOccurrenceOf ("=", false, false).unquoted();
            if (a.startsWith ("--plugin2="))  vst2File = juce::File (a.fromFirstOccurrenceOf ("=", false, false).unquoted());
        }

        // The test plugin's VST2 build sits next to its VST3 one (.../VST3/x.vst3 -> .../VST/).
        if (! vst2File.exists() && pluginFile.exists())
            for (auto& f : pluginFile.getParentDirectory().getParentDirectory().getChildFile ("VST")
                                     .findChildFiles (juce::File::findFiles, false, "*.dll;*.so"))
                vst2File = f;

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

        // Developer: --only=<text> runs just the steps whose name contains it (for the
        // measurement steps, which need no network); several texts: separated by "|".
        if (onlySteps.isNotEmpty())
        {
            const auto wanted = juce::StringArray::fromTokens (onlySteps, "|", {});
            steps.erase (std::remove_if (steps.begin(), steps.end(), [&wanted] (const Step& st)
                         {
                             for (auto& w : wanted)
                                 if (w.isNotEmpty() && st.name.contains (w))
                                     return false;

                             return true;
                         }),
                         steps.end());
        }

        startTimer (20);
    }

private:
    te::Engine& engine;
    std::function<void (int)> finished;
    juce::File pluginFile, vst2File, dumpDir;
    juce::String onlySteps;
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

    // 1.8: cutting, fades, stretch
    juce::String cutsHash;
    float duckRatio = 0.0f;
    bool loopRuleOk = false;
    juce::String groupSid;
    juce::StringArray compiledBeforeMarker;
    float cutsBaseline = 0.0f;
    juce::StringArray compiledBeforeCut;
    std::unique_ptr<Bouncer> testBouncer;
    Bouncer::Result bounceResult;
    bool bounceDone = false, bounceApplied = false;

    // Song files
    juce::File projectFile, legacyFile;
    appdata::MigrationResult migrationResult;
    juce::String migrationHash;
    project::SaveResult saveResult;
    bool saveDone = false, openDone = false;
    juce::String openError { "not done" }, newError { "not done" };
    int resyncsBefore = 0, channelsBefore = 0, clipsBefore = 0, insertsBefore = 0;
    bool eqEdited = false, limiterEdited = false, delayEdited = false, reverbEdited = false;

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

    // Punch in / out, templates
    juce::int64 fedAtTake3Start = -1;
    bool punchStoppedWhilePlaying = false, disarmCheck = false;
    juce::File templateFile;
    juce::String templateChoices, templateVocalsID;
    bool emptySongHadInput = false, scaleCheck = false;

    // FL Studio template import
    bool flpCheck = false, cleanupCheck = false, backupCheck = false;
    std::map<juce::String, bool> checks;   // results worked out once, in a step's action
    juce::String masterAutoSid;

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

    /** Renders some tracks (as the song plays them) between two times; returns the left channel
        (or the right one). */
    static juce::AudioBuffer<float> renderTracks (te::Edit& e, const juce::StringArray& trackNames, double from, double to,
                                                  bool rightSide = false)
    {
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("synctest-render", ".wav");
        juce::BigInteger tracks;
        auto all = te::getAllTracks (e);

        for (int i = 0; i < all.size(); ++i)
            if (trackNames.contains (all[i]->getName()))
                tracks.setBit (i);

        juce::AudioBuffer<float> result;
        const te::TimeRange range (te::TimePosition::fromSeconds (from), te::TimePosition::fromSeconds (to));

        // Like Renderer::renderToFile with a set of tracks, but without the master's effects
        // (earlier steps put a delay and a reverb there, which would blur these measurements).
        {
            const te::Edit::ScopedRenderStatus srs (e, true);
            te::Track::Array chosen;

            for (auto bit = tracks.findNextSetBit (0); bit != -1; bit = tracks.findNextSetBit (bit + 1))
                chosen.add (all[bit]);

            const te::FreezePointPlugin::ScopedTrackSoloIsolator isolator (e, chosen);
            te::TransportControl::stopAllTransports (e.engine, false, true);
            te::Renderer::Parameters r (e);
            r.destFile = file;
            r.audioFormat = e.engine.getAudioFileFormatManager().getDefaultFormat();
            r.bitDepth = 24;
            r.sampleRateForAudio = 44100.0;
            r.blockSizeForAudio = 512;
            r.time = range;
            r.usePlugins = true;
            r.useMasterPlugins = false;
            r.tracksToDo = tracks;

            if (auto task = te::render_utils::createRenderTask (r, "test", nullptr, nullptr))
                while (task->runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain) {}
        }

        if (file.existsAsFile())
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();

            if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (file) })
            {
                juce::AudioBuffer<float> both (2, (int) reader->lengthInSamples);
                reader->read (&both, 0, (int) reader->lengthInSamples, 0, true, true);
                result.setSize (1, both.getNumSamples());
                result.copyFrom (0, 0, both, rightSide && reader->numChannels > 1 ? 1 : 0, 0, both.getNumSamples());
            }
        }

        file.deleteFile();
        return result;
    }

    /** True once the engine can read every audio clip of these tracks (a compressed file
        first gets a cached WAV copy, made on the message thread: rendering before it
        exists, on the message thread, would wait for it forever). */
    static bool audioReady (te::Edit& e, const juce::StringArray& trackNames)
    {
        for (auto* t : te::getAudioTracks (e))
            if (trackNames.contains (t->getName()))
                for (auto* c : t->getClips())
                    if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
                        if (wc->getPlaybackFile().getSampleRate() <= 0.0)
                            return false;

        return true;
    }

    /** Peak level of part of a rendered buffer (seconds, at 44.1 kHz). */
    static float peakIn (const juce::AudioBuffer<float>& b, double from, double to)
    {
        const int start = juce::jlimit (0, b.getNumSamples(), (int) (from * 44100.0));
        const int end = juce::jlimit (start, b.getNumSamples(), (int) (to * 44100.0));
        return end > start ? b.getMagnitude (0, start, end - start) : 0.0f;
    }

    /** Pitch of a tone in part of a rendered buffer (Hz, by counting upward zero crossings). */
    static double frequencyIn (const juce::AudioBuffer<float>& b, double from, double to)
    {
        const int start = juce::jlimit (1, b.getNumSamples(), (int) (from * 44100.0));
        const int end = juce::jlimit (start, b.getNumSamples(), (int) (to * 44100.0));
        int first = -1, last = -1, count = 0;
        const float* d = b.getReadPointer (0);

        for (int i = start; i < end; ++i)
            if (d[i - 1] < 0.0f && d[i] >= 0.0f)
            {
                if (first < 0) first = i;
                last = i;
                ++count;
            }

        return count > 1 ? (count - 1) * 44100.0 / (double) (last - first) : 0.0;
    }

    /** Where the sound in a rendered buffer starts after a time (seconds; the first sample above a level). */
    static double soundStartIn (const juce::AudioBuffer<float>& b, double after, float level)
    {
        for (int i = juce::jmax (0, (int) (after * 44100.0)); i < b.getNumSamples(); ++i)
            if (std::abs (b.getSample (0, i)) > level)
                return i / 44100.0;

        return -1.0;
    }

    /** Where the sound in a rendered buffer stops (seconds; the last sample above -40 dB). */
    static double soundEndIn (const juce::AudioBuffer<float>& b)
    {
        for (int i = b.getNumSamples(); --i >= 0;)
            if (std::abs (b.getSample (0, i)) > 0.01f)
                return i / 44100.0;

        return 0.0;
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

        steps.push_back ({ "Playlist: 200 tracks - peer 2 places a clip on track 150 -> on track 150 everywhere",
            [this]
            {
                clipsBefore = song::getClips (H()).size();
                song::addClip (P2(), pat (P2(), "Pattern 1")[song::ids::sid].toString(), 149, 0.0);
            },
            [this]
            {
                return hashesEqual() && everywhere ([this] (te::Edit& e)
                {
                    int onTrack150 = 0;

                    for (auto& c : song::getClips (e))
                        if ((int) c[song::ids::lane] == 149)
                            ++onTrack150;

                    return song::getClips (e).size() == clipsBefore + 1 && onTrack150 == 1;
                });
            },
            8000, 600 });

        steps.push_back ({ "Playlist: peer 2 removes the track-150 clip again -> gone everywhere",
            [this]
            {
                for (auto& c : song::getClips (P2()))
                    if ((int) c[song::ids::lane] == 149)
                    {
                        song::removeClip (P2(), c);
                        break;
                    }
            },
            [this]
            {
                return hashesEqual() && everywhere ([this] (te::Edit& e) { return song::getClips (e).size() == clipsBefore; });
            },
            8000, 600 });

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

        // Punch in / out (like FL): only the range is kept; recording stops by itself at the punch-out.
        steps.push_back ({ "Punch in/out: range beats 24-28, 1 bar pre-roll; peer 2 presses Rec -> the song starts about a bar early (no click), recording",
            [this]
            {
                Recorder::Punch punch;
                punch.on = true;
                punch.inBeat = 24.0;
                punch.outBeat = 28.0;
                punch.preRollBars = 1;
                peer2.getRecorder().setPunch (punch);
                soundCard->startFeeding (0.5f, P2());
                juce::Thread::sleep (100);
                recordStarted = peer2.startRecording (timeOfBeat (P2(), 0.0));   // (the start marker doesn't count with punch on)
            },
            [this]
            {
                const double pos = soundCard->lastPosition();
                return recordStarted && peer2.isRecording() && pos >= timeOfBeat (P2(), 19.0) && pos < timeOfBeat (P2(), 23.0);
            },
            8000 });

        steps.push_back ({ "Punch: past the punch-out recording stops by itself while the song plays on; 'Vocals take 3' covers exactly beats 24-28",
            {},
            [this]
            {
                const double pos = soundCard->lastPosition();

                if (fedAtTake3Start < 0)
                {
                    if (pos < timeOfBeat (P2(), 24.0) + 0.3)
                        return false;

                    fedAtTake3Start = soundCard->inputSampleAt (timeOfBeat (P2(), 24.0));
                    return false;
                }

                if (! punchStoppedWhilePlaying)
                {
                    if (pos < timeOfBeat (P2(), 28.0) + 0.4)
                        return false;

                    punchStoppedWhilePlaying = ! peer2.isRecording() && P2().getTransport().isPlaying();
                    std::cout << "    at beat " << P2().tempoSequence.toBeats (te::TimePosition::fromSeconds (pos)).inBeats()
                              << ": recording " << (peer2.isRecording() ? "STILL ON" : "stopped") << ", song "
                              << (P2().getTransport().isPlaying() ? "playing on" : "STOPPED") << std::endl;

                    if (! punchStoppedWhilePlaying)
                        return false;

                    peer2.setPlaying (false, timeOfBeat (P2(), 0.0));
                }

                auto clip = clipOfChannel (P2(), "Vocals take 3");

                if (! clip.isValid())
                    return false;

                soundCard->stopFeeding();
                const double expectedLength = timeOfBeat (P2(), 28.0) - timeOfBeat (P2(), 24.0);
                std::cout << "    take 3: start beat " << (double) clip[song::ids::start] << ", " << (double) clip[song::ids::duration]
                          << " s (punch range " << expectedLength << " s), offset " << (double) clip[song::ids::offset] << " s" << std::endl;

                return std::abs ((double) clip[song::ids::start] - 24.0) < 0.01
                    && std::abs ((double) clip[song::ids::duration] - expectedLength) < 0.01
                    && hasVerified (host, clip[song::ids::asset].toString()) && hashesEqual();
            },
            30000 });

        steps.push_back ({ "Punch: take 3 lines up to the sample with what was playing at the punch-in (host's copy)",
            {},
            [this]
            {
                auto clip = clipOfChannel (H(), "Vocals take 3");
                auto file = host.getAssetStore().getFile (clip[song::ids::asset].toString());
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));

                // The file sample the clip starts on (its offset) must be the input sample
                // that went in while the song played the punch-in.
                const auto offsetSamples = (juce::int64) std::llround ((double) clip[song::ids::offset] * SimulatedSoundCard::sampleRate);
                juce::int64 found = -1;

                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> b (1, 256);
                    reader->read (&b, 0, 256, offsetSamples, true, false);
                    double bestError = 1.0e9;

                    for (juce::int64 k = fedAtTake3Start - 4096; k < fedAtTake3Start + 4096; ++k)
                    {
                        double err = 0.0;

                        for (int j = 0; j < 256; ++j)
                            err += std::abs (b.getSample (0, j) - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 437.31
                                                                                   * (double) (k + j) / SimulatedSoundCard::sampleRate));

                        if (err < bestError)
                        {
                            bestError = err;
                            found = k;
                        }
                    }
                }

                std::cout << "    the clip starts on input sample " << found << "; the song was at the punch-in at input sample "
                          << fedAtTake3Start << " -> " << (found == fedAtTake3Start ? "lined up exactly" : "OFF") << std::endl;
                peer2.getRecorder().setPunch ({});
                return found == fedAtTake3Start;
            } });

        steps.push_back ({ "Recording: Disarm all disarms (the input stays), Arm all arms again; monitoring off and on; nothing shared changes",
            [this]
            {
                auto& rec = peer2.getRecorder();
                rec.disarmAll();
                disarmCheck = rec.getNumArmed() == 0 && rec.getDisarmedWithInput().size() == 1
                              && rec.getInputFor (vocalsID).isNotEmpty();
                rec.setMonitoring (false);
                rec.setMonitoring (true);
                rec.armAllWithInput();
                disarmCheck = disarmCheck && rec.getNumArmed() == 1 && rec.isArmed (vocalsID);
            },
            [this] { return disarmCheck && hashesEqual(); } });

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

        // Templates: a song to start new ones from, with this PC's recording inputs.
        steps.push_back ({ "Template: the 4th PC gives insert 'Vocals' an input and saves the song as a template (folder + Inputs.txt)",
            [this]
            {
                auto* vocals = insert (*fourth.getEdit(), "Vocals");
                templateVocalsID = vocals != nullptr ? vocals->itemID.toString() : juce::String();
                fourth.getRecorder().setInputFor (templateVocalsID, "test-mic");
                templateChoices = fourth.getRecorder().saveChoices (true);
                templateFile = testRoot.getChildFile ("Templates").getChildFile ("Recording").getChildFile ("Recording.cotosong");
                saveDone = false;
                fourth.saveProject (templateFile, true, [this] (project::SaveResult r)
                {
                    saveResult = r;
                    templateFile.getSiblingFile ("Inputs.txt").replaceWithText (templateChoices);
                    saveDone = true;
                });
            },
            [this]
            {
                return saveDone && saveResult.ok() && templateFile.existsAsFile()
                    && templateFile.getSiblingFile ("Inputs.txt").loadFileAsString().contains ("test-mic")
                    && templateChoices.contains ("Vocals");
            },
            30000 });

        steps.push_back ({ "Template: a new empty song forgets the input (no 'Vocals' insert); a new song from the template is the same song, 'Untitled', input back",
            [this]
            {
                fourth.newProject();
                emptySongHadInput = fourth.getRecorder().getInputFor (templateVocalsID).isNotEmpty();
                openDone = false;
                fourth.openProject (templateFile, [this] (juce::String error)
                {
                    openError = error;
                    fourth.getRecorder().restoreChoices (templateFile.getSiblingFile ("Inputs.txt").loadFileAsString());
                    openDone = true;
                }, "Untitled");
            },
            [this]
            {
                if (! openDone || fourth.getEdit() == nullptr)
                    return false;

                auto& e = *fourth.getEdit();
                auto* vocals = insert (e, "Vocals");
                const bool sameParts = song::getChannels (e).size() == song::getChannels (H()).size()
                                    && song::getInserts (e).size() == song::getInserts (H()).size()
                                    && song::getPatterns (e).size() == song::getPatterns (H()).size();
                std::cout << "    title \"" << song::getTitle (e) << "\", same channels/inserts/patterns as the song: " << (sameParts ? "yes" : "NO")
                          << ", empty song had the input: " << (emptySongHadInput ? "YES" : "no")
                          << ", input back on 'Vocals': " << fourth.getRecorder().getInputFor (templateVocalsID) << std::endl;

                return openError.isEmpty() && ! emptySongHadInput && sameParts && song::getTitle (e) == "Untitled"
                    && vocals != nullptr && vocals->itemID.toString() == templateVocalsID
                    && fourth.getRecorder().getInputFor (templateVocalsID) == "test-mic" && fourth.getRecorder().isArmed (templateVocalsID);
            },
            30000 });

        steps.push_back ({ "FL template import: an .flp's inserts, routing, sends, faders, effects (VST3 with its saved settings), tempo and track names",
            [this]
            {
                // The test plugin's VST3 state with its knob at 0.3: what FL would have saved.
                juce::PluginDescription gainDesc;
                juce::MemoryBlock component, controller;

                if (pluginFile.exists())
                {
                    juce::VST3PluginFormat format;
                    juce::OwnedArray<juce::PluginDescription> found;
                    format.findAllTypesForFile (found, pluginFile.getFullPathName());

                    if (! found.isEmpty())
                    {
                        gainDesc = *found[0];
                        engine.getPluginManager().knownPluginList.addType (gainDesc);
                        juce::String error;

                        if (auto instance = format.createInstanceFromDescription (gainDesc, 44100.0, 512, error))
                        {
                            instance->getParameters()[0]->setValue (0.3f);
                            juce::MemoryBlock state;
                            instance->getStateInformation (state);

                            if (auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()))
                            {
                                if (auto* c = xml->getChildByName ("IComponent"))     component.fromBase64Encoding (c->getAllSubText());
                                if (auto* c = xml->getChildByName ("IEditController")) controller.fromBase64Encoding (c->getAllSubText());
                            }
                        }
                    }
                }

                const bool havePlugin = component.getSize() > 0;

                // master; 1 "Vocals" -> 2 "Bus" (main route) + 4 "FX" (closed send);
                // 2 "Bus" -> master at FL's 16000 (+5.6 dB); 3 unnamed, empty, plain: not used; 4 "FX" -> master.
                FlpWriter w;
                auto version = FlpWriter::text8 ("26.1.5.0");
                version.append ("\0", 1);
                w.dataEvent (199, version);
                w.dwordEvent (156, 140000);
                w.byteEvent (17, 4);
                w.byteEvent (18, 4);

                for (auto [track, name] : { std::pair<int, const char*> { 1, "GRABAR VOCALES" }, { 3, "CHORUS 1" } })
                {
                    juce::MemoryBlock info (70, true);
                    info.copyFrom (&track, 0, 4);

                    // Track colours (R, G, B, 0): track 1 keeps FL's default grey, track 3 is pink.
                    const juce::uint8 grey[] = { 0x48, 0x51, 0x56, 0 }, pink[] = { 200, 40, 90, 0 };
                    info.copyFrom (track == 1 ? grey : pink, 4, 4);
                    w.dataEvent (238, info);
                    w.textEvent (239, name);
                }

                // A channel (its plugin must not end up in the master's slots).
                w.textEvent (201, "Fruity Wrapper");
                w.dataEvent (212, juce::MemoryBlock (52, true));

                auto slot = [&w] (int number) { w.wordEvent (98, number); };
                auto header = [&w] (const juce::String& name)
                {
                    if (name.isNotEmpty())
                        w.textEvent (204, name);
                    w.dataEvent (236, juce::MemoryBlock (12, true));
                };
                auto routing = [&w] (std::initializer_list<int> to)
                {
                    juce::MemoryBlock r (8, true);
                    for (auto t : to)
                        r[(size_t) t] = 1;
                    w.dataEvent (235, r);
                };

                header ("The Master");
                w.textEvent (201, "Fruity Limiter");  slot (0);
                for (int i = 1; i < 10; ++i) slot (i);
                routing ({});

                header ("Vocals");
                {
                    juce::MemoryOutputStream wrapper, state;
                    wrapper.writeInt (12);
                    juce::MemoryBlock kind (4, true);  kind[0] = 7;
                    FlpWriter::record (wrapper, 50, kind);
                    FlpWriter::record (wrapper, 54, FlpWriter::text8 (gainDesc.name.isNotEmpty() ? gainDesc.name : juce::String ("SyncTestGain")));
                    FlpWriter::record (wrapper, 55, FlpWriter::text8 (pluginFile.getFullPathName()));
                    FlpWriter::record (wrapper, 56, FlpWriter::text8 ("SyncTest contributors"));
                    state.writeRepeatedByte (0, 80);
                    FlpWriter::record (state, 3, component);
                    FlpWriter::record (state, 2, controller);
                    FlpWriter::record (wrapper, 53, state.getMemoryBlock());

                    w.textEvent (201, "Fruity Wrapper");
                    w.dataEvent (212, juce::MemoryBlock (52, true));
                    w.textEvent (203, "my gain");
                    w.dataEvent (213, wrapper.getMemoryBlock());
                    slot (0);
                }
                {
                    // The same plugin renamed "brighter" inside a "WaveShell": only its VST3 class ID can
                    // find it (like a renamed Waves plugin). A JUCE plugin's class: 0xABCDEF01 0x9182FAEB
                    // <maker code> <plugin code>, laid out in memory the way this platform does it.
                    const juce::uint32 words[4] = { 0xABCDEF01u, 0x9182FAEBu, 0x53796E74u /* Synt */, 0x5374476Eu /* StGn */ };
                    juce::MemoryBlock cid (16, true);
                    auto* b = (juce::uint8*) cid.getData();
                    auto bigEndian = [b] (int at, juce::uint32 v) { for (int i = 0; i < 4; ++i) b[at + i] = (juce::uint8) (v >> (24 - 8 * i)); };
                   #if JUCE_WINDOWS
                    for (int i = 0; i < 4; ++i) b[i] = (juce::uint8) (words[0] >> (8 * i));                // COM layout
                    b[4] = (juce::uint8) (words[1] >> 16);  b[5] = (juce::uint8) (words[1] >> 24);
                    b[6] = (juce::uint8) words[1];          b[7] = (juce::uint8) (words[1] >> 8);
                   #else
                    bigEndian (0, words[0]);
                    bigEndian (4, words[1]);
                   #endif
                    bigEndian (8, words[2]);
                    bigEndian (12, words[3]);

                    juce::MemoryOutputStream wrapper, state;
                    wrapper.writeInt (12);
                    juce::MemoryBlock kind (4, true);  kind[0] = 7;
                    FlpWriter::record (wrapper, 50, kind);
                    FlpWriter::record (wrapper, 52, cid);
                    FlpWriter::record (wrapper, 54, FlpWriter::text8 ("WaveShell1-VST3 99.0_x64"));
                    FlpWriter::record (wrapper, 55, FlpWriter::text8 ("C:\\Program Files\\Common Files\\VST3\\WaveShell1-VST3 99.0_x64.vst3"));
                    state.writeRepeatedByte (0, 80);
                    FlpWriter::record (state, 3, component);
                    FlpWriter::record (wrapper, 53, state.getMemoryBlock());

                    w.textEvent (201, "Fruity Wrapper");
                    w.textEvent (203, "brighter");
                    w.dataEvent (213, wrapper.getMemoryBlock());
                    slot (1);
                }
                w.textEvent (201, "Fruity Parametric EQ 2");  slot (2);
                w.textEvent (201, "Fruity Reeverb 2");        slot (3);
                for (int i = 4; i < 10; ++i) slot (i);
                routing ({ 2, 4 });

                header ("Bus");   for (int i = 0; i < 10; ++i) slot (i);  routing ({ 0 });
                header ({});      for (int i = 0; i < 10; ++i) slot (i);  routing ({ 0 });
                header ("FX");    for (int i = 0; i < 10; ++i) slot (i);  routing ({ 0 });

                juce::MemoryOutputStream params;
                auto param = [&params] (int what, int kind, int insert, int sub, int value)
                {
                    params.writeInt (0);
                    params.writeByte ((char) what);
                    params.writeByte ((char) kind);
                    params.writeShort ((short) (0x7000 + insert * 64 + sub));
                    params.writeInt (value);
                };
                param (0xc0, 0x1f, 2, 0, 16000);    // Bus fader at FL's maximum
                param (0, 0x1f, 1, 2, 0);           // Vocals slot 2 (the EQ) off
                param (4, 0x20, 1, 0, 0);           // Vocals -> FX closed
                w.dataEvent (225, params.getMemoryBlock());

                // Read it back and build a song from it.
                flimport::Project project;
                const auto error = flimport::parse (w.finish(), project);
                const auto used = project.getUsedInserts();

                auto edit = te::createEmptyEdit (engine, juce::File());
                for (auto* t : te::getAudioTracks (*edit))
                    edit->deleteTrack (t);

                flimport::Options options;
                options.inserts = used;
                const auto report = flimport::apply (*edit, project, options);

                auto* vocals = insert (*edit, "Vocals");
                auto* bus = insert (*edit, "Bus");
                auto* fx = insert (*edit, "FX");
                auto vocalSends = vocals != nullptr ? song::getSendsFrom (*edit, vocals->itemID.toString()) : juce::Array<juce::ValueTree>();
                auto effects = vocals != nullptr ? song::getInsertEffects (*vocals) : juce::Array<te::Plugin*>();
                const float busDb = bus != nullptr ? song::getVolumePlugin (*bus)->getVolumeDb() : -1000.0f;

                auto gainOf = [&effects] (int index)
                {
                    if (auto* ext = dynamic_cast<te::ExternalPlugin*> (effects[index]))
                        if (auto* pi = ext->getAudioPluginInstance())
                            return pi->getParameters()[0]->getValue();

                    return -1.0f;
                };

                const float gain = gainOf (0), gainByClass = gainOf (1);
                // Vocals: [gain, brighter,] the EQ (off, as in FL), then our Reverb for Fruity Reeverb 2.
                const int eqIndex = effects.size() - 2;
                const bool eqOff = effects.size() == (havePlugin ? 4 : 2) && eqIndex >= 0
                                && ! effects[eqIndex]->isEnabled()
                                && effects[eqIndex]->getPluginType() == juce::String (ParametricEQPlugin::xmlTypeName);
                const bool reverbIn = ! effects.isEmpty() && effects.getLast()->isEnabled()
                                && effects.getLast()->getPluginType() == juce::String (ReverbPlugin::xmlTypeName);
                bool limiterOnMaster = false;

                for (auto* mp : edit->getMasterPluginList())
                    limiterOnMaster = limiterOnMaster || dynamic_cast<LimiterPlugin*> (mp) != nullptr;

                std::cout << "    parse: " << (error.isEmpty() ? "ok" : error) << ", " << project.bpm << " BPM, used inserts "
                          << juce::String (used.size()) << ", inserts made " << report.insertsMade
                          << ", Vocals -> " << (vocals != nullptr && bus != nullptr && song::getRoute (*vocals) == bus->itemID.toString() ? "Bus" : "?")
                          << ", sends " << vocalSends.size() << (vocalSends.size() == 1 ? " at " + juce::String (song::getSendLevel (vocalSends[0])) + " dB" : juce::String())
                          << ", Bus fader " << busDb << " dB, gain knob " << gain << " / found by class ID " << gainByClass
                          << " (want 0.3), EQ in and off: " << (eqOff ? "yes" : "NO")
                          << ", our Reverb for Reeverb 2: " << (reverbIn ? "yes" : "NO")
                          << ", our Limiter on the master: " << (limiterOnMaster ? "yes" : "NO")
                          << ", not available: " << report.notAvailable.joinIntoString ("; ")
                          << ", track 1 \"" << song::getLaneName (*edit, 0) << "\", track 3 \"" << song::getLaneName (*edit, 2) << "\""
                          << " colours " << song::getLaneColour (*edit, 0).toString() << " / " << song::getLaneColour (*edit, 2).toString() << std::endl;

                flpCheck = error.isEmpty() && std::abs (project.bpm - 140.0) < 1.0e-6
                        && used == juce::Array<int> { 1, 2, 4 } && report.insertsMade == 3
                        && vocals != nullptr && bus != nullptr && fx != nullptr && song::getInserts (*edit).size() == 3
                        && song::getRoute (*vocals) == bus->itemID.toString() && song::getRoute (*bus).isEmpty()
                        && vocalSends.size() == 1 && vocalSends[0][song::ids::to].toString() == fx->itemID.toString()
                        && std::abs (song::getSendLevel (vocalSends[0]) - song::minSendDb) < 1.0e-6
                        && std::abs (busDb - 5.6f) < 0.05f
                        && eqOff
                        && (! havePlugin || (std::abs (gain - 0.3f) < 1.0e-3f && std::abs (gainByClass - 0.3f) < 1.0e-3f))
                        && (! havePlugin || report.effectsWithSettings == 2)
                        && reverbIn && limiterOnMaster
                        && report.notAvailable.isEmpty()   // Fruity Limiter and Reeverb 2 now have ours
                        && edit->tempoSequence.getTempo (0)->getBpm() == 140.0
                        && song::getLaneName (*edit, 0) == "GRABAR VOCALES" && song::getLaneName (*edit, 2) == "CHORUS 1"
                        && song::getLaneName (*edit, 1).isEmpty()
                        && song::getLaneColour (*edit, 0).isTransparent() && song::getLaneColour (*edit, 2) == juce::Colour (200, 40, 90);
            },
            [this] { return flpCheck; },
            30000 });

        steps.push_back ({ "Playlist track names: the host names track 6 -> every PC shows it",
            [this] { song::setLaneName (H(), 5, "Hook"); },
            [this] { return everywhere ([] (te::Edit& e) { return song::getLaneName (e, 5) == "Hook"; }); } });

        steps.push_back ({ "Playlist track names: clearing the name removes it on every PC",
            [this] { song::setLaneName (H(), 5, {}); },
            [this] { return everywhere ([] (te::Edit& e) { return song::getLaneName (e, 5).isEmpty(); }); } });

        steps.push_back ({ "Playlist tracks: the host colours track 6 and sends its takes to the master -> every PC has both",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                song::setLaneColour (H(), 5, juce::Colour (0xff3fa2db));
                song::setLaneInsertSetting (H(), 5, song::laneToMaster);
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    return song::getLaneColour (e, 5) == juce::Colour (0xff3fa2db) && song::getLaneInsertSetting (e, 5) == song::laneToMaster;
                });
            } });

        steps.push_back ({ "Playlist tracks: clearing the colour and the setting removes the track's entry on every PC",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                song::setLaneColour (H(), 5, juce::Colours::transparentBlack);
                song::setLaneInsertSetting (H(), 5, {});
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    int entries = 0;

                    for (auto l : song::getModel (e).getChildWithName (song::ids::LANES))
                        if ((int) l[song::ids::lane] == 5)
                            ++entries;

                    return entries == 0 && song::getLaneColour (e, 5).isTransparent();
                });
            } });

        steps.push_back ({ "Takes follow their track: dropped on a track named like an insert (or set to one) they play through it; 'leave as is' keeps it; new takes go to their insert's track",
            [] {},
            [this]
            {
                auto edit = te::createEmptyEdit (engine, juce::File());
                auto* chorus = song::addInsert (*edit, "CHORUS 1");
                auto* verse = song::addInsert (*edit, "VERSO 1");
                auto* vocals = song::addInsert (*edit, "GRABAR VOCALES");

                if (chorus == nullptr || verse == nullptr || vocals == nullptr)
                    return false;

                const auto chorusID = chorus->itemID.toString(), verseID = verse->itemID.toString(), vocalsID = vocals->itemID.toString();
                song::setLaneName (*edit, 1, "GRABAR VOCALES");
                song::setLaneName (*edit, 2, "chorus 1");                 // same name, other case: linked
                song::setLaneInsertSetting (*edit, 3, verseID);           // set by hand
                song::setLaneName (*edit, 4, "VERSO 1");
                song::setLaneInsertSetting (*edit, 4, song::laneKeepsInsert);   // named like one, but told to leave takes alone
                song::setLaneInsertSetting (*edit, 5, song::laneToMaster);

                auto* take = song::addAudioChannel (*edit, "GRABAR VOCALES take 1");
                song::setRoute (*edit, *take, vocalsID);
                auto clip = song::addAudioClip (*edit, "0123456789abcdef0123456789abcdef", song::getChannelID (*take), 0, 0.0, 0.0, 2.0);

                auto routeAfterMoveTo = [&] (int lane)
                {
                    clip.setProperty (song::ids::lane, lane, nullptr);
                    song::routeTakeToLane (*edit, clip);
                    return song::getRoute (*take);
                };

                const auto onChorus = routeAfterMoveTo (2);
                const auto onKeep = routeAfterMoveTo (4);
                const auto onVerse = routeAfterMoveTo (3);
                const auto onPlain = routeAfterMoveTo (8);   // a track with no setting and no matching insert
                const auto onMaster = routeAfterMoveTo (5);

                // A new take recorded through GRABAR VOCALES lands on the free track linked to it (track 2).
                AssetStore::ImportResult r;
                r.hash = "fedcba9876543210fedcba9876543210";
                r.lengthSeconds = 1.0;
                r.sampleRate = 44100.0;
                r.numChannels = 1;
                auto recorded = song::addRecordedTake (*edit, r, vocalsID, 0.0, 0.0, 1.0);
                const int recordedLane = recorded[song::ids::lane];

                std::cout << "    chorus " << (onChorus == chorusID) << ", keep " << (onKeep == chorusID) << ", verse " << (onVerse == verseID)
                          << ", plain track " << (onPlain == verseID) << ", master " << onMaster.isEmpty() << ", new take on track " << (recordedLane + 1) << std::endl;
                edit = nullptr;
                return onChorus == chorusID && onKeep == chorusID && onVerse == verseID && onPlain == verseID
                    && onMaster.isEmpty() && recordedLane == 1;
            } });

        steps.push_back ({ "Scale helper: notes snap to the scale (C major, A minor, C minor pentatonic), off-scale ties go down unless dragging up",
            [this]
            {
                scaleCheck = scales::nearest (0, 0, 61) == 60 && scales::nearest (0, 0, 61, true) == 62     // C# in C major
                          && scales::contains (9, 1, 60) && ! scales::contains (9, 1, 61)                       // A minor: C yes, C# no
                          && scales::nearest (0, 10, 64) == 63 && scales::nearest (0, 10, 64, true) == 65      // E in C minor pentatonic
                          && scales::nearest (0, 0, 64) == 64                                                   // already in the scale
                          && scales::describe (9, 1) == "A minor";
            },
            [this] { return scaleCheck; } });

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

        steps.push_back ({ "Song file: afterwards peer 1 adds a channel and peer 2 a note -> everywhere, no ID clashes; the channel plays into the master (no insert made for it)",
            [this]
            {
                channelsBefore = song::getChannels (H()).size();
                insertsBefore = song::getInserts (H()).size();
                song::addChannel (P1(), song::InstrumentKind::synth);
                song::setStep (P2(), pat (P2(), "Beat - Kick"), *ch (P2(), "Kick"), 6, true);
            },
            [this]
            {
                return hashesEqual() && compiledEqualEverywhere()
                    && everywhere ([this] (te::Edit& e)
                       {
                           auto channels = song::getChannels (e);
                           return channels.size() == channelsBefore + 1 && idsUnique (e)
                               && hasStep (e, "Beat - Kick", "Kick", 6)
                               && song::getRoute (*channels.getLast()).isEmpty()
                               && song::getInserts (e).size() == insertsBefore;
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

        steps.push_back ({ "Edit sound: peer 2 trims the 808 (end 40%), fades it out 50 ms, turns on the envelope and a 800 Hz low pass -> same everywhere",
            [this]
            {
                if (auto* t = ch (P2(), "808 long"))
                    if (auto* s = dynamic_cast<SamplerPlugin*> (song::getInstrument (*t)))
                    {
                        P2().getUndoManager().beginNewTransaction();
                        s->endParam->setParameter (0.4f, juce::sendNotification);
                        s->fadeOutParam->setParameter (50.0f, juce::sendNotification);
                        s->envOnParam->setParameter (1.0f, juce::sendNotification);
                        s->filterTypeParam->setParameter (1.0f, juce::sendNotification);
                        s->cutoffParam->setParameter (800.0f, juce::sendNotification);
                    }
            },
            [this]
            {
                return hashesEqual() && everywhere ([] (te::Edit& e)
                {
                    auto* t = ch (e, "808 long");
                    auto* s = t != nullptr ? dynamic_cast<SamplerPlugin*> (song::getInstrument (*t)) : nullptr;
                    return s != nullptr && std::abs (s->endParam->getCurrentValue() - 0.4f) < 1.0e-4f
                        && std::abs (s->fadeOutParam->getCurrentValue() - 50.0f) < 1.0e-3f
                        && s->envOnParam->getCurrentValue() > 0.5f && juce::roundToInt (s->filterTypeParam->getCurrentValue()) == 1
                        && std::abs (s->cutoffParam->getCurrentValue() - 800.0f) < 0.5f && s->cutValue.get();
                });
            },
            8000, 600 });

        steps.push_back ({ "Edit sound: measured (trim, reverse, fades, tune, envelope and release at the note's end, one-shot without it, stop at note end, cut itself, filters)",
            [] {},
            [this]
            {
                const double sr = 44100.0;
                const int n = 44100;
                bool ok = true;
                auto check = [&ok] (const juce::String& what, double got, double lo, double hi)
                {
                    const bool good = got >= lo && got <= hi && ! std::isnan (got);
                    std::cout << "    " << what << ": " << juce::String (got, 5) << (good ? "" : "   <-- expected " + juce::String (lo) + " .. " + juce::String (hi)) << std::endl;
                    ok = ok && good;
                };

                // Two test sounds: a ramp 0 -> 1 over one second (each sample tells where it was read), and a 5 kHz sine.
                auto writeWav = [] (const juce::File& f, std::function<float (int)> wave, int length)
                {
                    juce::AudioBuffer<float> b (1, length);
                    for (int i = 0; i < length; ++i) b.setSample (0, i, wave (i));
                    f.deleteFile();
                    std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (new juce::FileOutputStream (f), 44100.0, 1, 32, {}, 0));
                    if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, length);
                };
                const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("cotostudio-sampler-test");
                dir.createDirectory();
                writeWav (dir.getChildFile ("ramp.wav"), [n] (int i) { return (float) i / (float) n; }, n);
                writeWav (dir.getChildFile ("sine.wav"), [] (int i) { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 5000.0 * i / 44100.0); }, n);

                auto edit = te::createEmptyEdit (engine, juce::File());
                auto plugin = edit->getPluginCache().createNewPlugin (SamplerPlugin::xmlTypeName, {});
                auto* s = dynamic_cast<SamplerPlugin*> (plugin.get());

                if (s == nullptr)
                    return false;

                s->initialise ({ te::TimePosition(), sr, 512 });
                s->loadSample ("ramp", dir.getChildFile ("ramp.wav"));

                auto defaults = [s]
                {
                    for (auto* p : s->params())
                        p->setParameter (p->getDefaultValue().value_or (0.0f), juce::sendNotificationSync);

                    s->cutValue = false;
                    s->noteLengthValue = false;
                };

                // Plays notes (sample position, on / off) and returns the left output.
                auto render = [s, sr] (int total, std::vector<std::pair<int, bool>> notes)
                {
                    s->reset();
                    juce::AudioBuffer<float> buffer (2, 512);
                    te::MidiMessageArray midi;
                    std::vector<float> out;

                    for (int pos = 0; pos < total; pos += 512)
                    {
                        midi.clear();

                        for (auto& note : notes)
                            if (note.first >= pos && note.first < pos + 512)
                                midi.addMidiMessage (note.second ? juce::MidiMessage::noteOn (1, 60, 1.0f) : juce::MidiMessage::noteOff (1, 60),
                                                     (note.first - pos) / sr, te::MPESourceID {});

                        buffer.clear();
                        te::PluginRenderContext rc (&buffer, 0, 512, &midi, 0.0, {}, true, false, false, false);
                        s->applyToBuffer (rc);

                        for (int i = 0; i < 512; ++i)
                            out.push_back (buffer.getSample (0, i));
                    }

                    return out;
                };

                auto ramp = [n] (double i) { return i / n; };
                const std::vector<std::pair<int, bool>> hit { { 0, true } };

                defaults();
                auto o = render (n + 2048, hit);
                check ("plain: sample 1000 plays the file's sample 1000", o[1000], ramp (1000) - 1e-5, ramp (1000) + 1e-5);
                check ("plain: the whole file plays, then silence (sample 44200)", o[30000] - ramp (30000) + std::abs (o[44200]), -1e-4, 1e-4);

                defaults(); s->startParam->setParameter (0.5f, juce::sendNotificationSync);
                o = render (4096, hit);
                check ("start 50%: first sample", o[0], 0.4999, 0.5001);

                defaults(); s->endParam->setParameter (0.25f, juce::sendNotificationSync);
                o = render (16384, hit);
                check ("end 25%: still playing at 11000", o[11000], ramp (11000) - 1e-4, ramp (11000) + 1e-4);
                check ("end 25%: silent from 11030 (250 ms)", std::abs (o[11030]) + std::abs (o[12000]), 0.0, 1e-9);

                defaults(); s->reverseParam->setParameter (1.0f, juce::sendNotificationSync);
                o = render (n + 2048, hit);
                check ("reverse: starts at the file's end", o[0], 0.9995, 1.0001);
                check ("reverse: half way = the file's middle", o[22050], 0.4995, 0.5005);

                defaults(); s->reverseParam->setParameter (1.0f, juce::sendNotificationSync); s->endParam->setParameter (0.5f, juce::sendNotificationSync);
                o = render (n, hit);
                check ("reverse + end 50%: plays the file's second half backwards, then stops", (o[22000] > 0.49 ? 1.0 : 0.0) + std::abs (o[22100]), 0.9999, 1.0001);

                defaults(); s->fadeInParam->setParameter (100.0f, juce::sendNotificationSync);
                o = render (8192, hit);
                check ("fade in 100 ms: half volume at 50 ms", o[2205] / ramp (2205), 0.49, 0.51);
                check ("fade in 100 ms: full volume at 150 ms", o[6615] / ramp (6615), 0.999, 1.001);

                defaults(); s->fadeOutParam->setParameter (100.0f, juce::sendNotificationSync);
                o = render (n + 2048, hit);
                check ("fade out 100 ms: half volume 50 ms before the end", o[n - 1 - 2205] / ramp (n - 1 - 2205), 0.49, 0.51);

                defaults(); s->tuneParam->setParameter (12.0f, juce::sendNotificationSync);
                o = render (n, hit);
                check ("tune +12: twice as fast (sample 1000 plays 2000)", o[1000], ramp (2000) - 1e-4, ramp (2000) + 1e-4);
                check ("tune +12: over after half a second", std::abs (o[22060]), 0.0, 1e-9);

                defaults(); s->fineParam->setParameter (100.0f, juce::sendNotificationSync);
                o = render (4096, hit);
                check ("fine +100 cents = one semitone", o[1000], ramp (1000 * std::pow (2.0, 1.0 / 12.0)) - 1e-4, ramp (1000 * std::pow (2.0, 1.0 / 12.0)) + 1e-4);

                const std::vector<std::pair<int, bool>> shortNote { { 0, true }, { 4410, false } };
                defaults();
                o = render (16384, shortNote);
                check ("envelope off: a 100 ms note still plays the whole sound (one-shot)", o[8820], ramp (8820) - 1e-4, ramp (8820) + 1e-4);

                defaults(); s->noteLengthValue = true;
                o = render (16384, shortNote);
                check ("stop at note end: full volume while the note lasts", o[4000] / ramp (4000), 0.999, 1.001);
                check ("stop at note end: half volume 5 ms after the note ends (10 ms fade)", o[4410 + 220] / ramp (4410 + 220), 0.47, 0.53);
                check ("stop at note end: silent from 10 ms after it", std::abs (o[4410 + 450]) + std::abs (o[8820]), 0.0, 1e-9);

                defaults(); s->envOnParam->setParameter (1.0f, juce::sendNotificationSync); s->releaseParam->setParameter (100.0f, juce::sendNotificationSync);
                o = render (16384, shortNote);
                check ("envelope on: full volume while the note lasts", o[4000] / ramp (4000), 0.999, 1.001);
                check ("release 100 ms: half volume 50 ms after the note ends", o[4410 + 2205] / ramp (4410 + 2205), 0.48, 0.52);
                check ("release 100 ms: silent after it", std::abs (o[4410 + 4410 + 50]) + std::abs (o[12000]), 0.0, 1e-9);

                defaults(); s->envOnParam->setParameter (1.0f, juce::sendNotificationSync); s->attackParam->setParameter (100.0f, juce::sendNotificationSync);
                o = render (8192, hit);
                check ("attack 100 ms: half volume at 50 ms", o[2205] / ramp (2205), 0.49, 0.51);

                defaults(); s->envOnParam->setParameter (1.0f, juce::sendNotificationSync);
                s->decayParam->setParameter (100.0f, juce::sendNotificationSync); s->sustainParam->setParameter (0.5f, juce::sendNotificationSync);
                o = render (12288, hit);
                check ("decay 100 ms to sustain 50%: 75% half way", o[2205] / ramp (2205), 0.74, 0.76);
                check ("sustain 50% held", o[10000] / ramp (10000), 0.499, 0.501);

                const std::vector<std::pair<int, bool>> twoHits { { 0, true }, { 1000, true } };
                defaults();
                o = render (4096, twoHits);
                check ("without cut itself: both hits sound together", o[3000], ramp (3000) + ramp (2000) - 1e-4, ramp (3000) + ramp (2000) + 1e-4);
                s->cutValue = true;
                o = render (4096, twoHits);
                check ("cut itself: only the new hit after 5 ms", o[3000], ramp (2000) - 1e-4, ramp (2000) + 1e-4);

                // 1.8 note properties: pan / fine pitch arrive as key pressure on channel 16 / 15
                // for the note's key at its start (in any order with the note-on); release
                // velocity is the note-off's velocity.
                auto renderMessages = [s, sr] (int total, std::vector<std::pair<int, juce::MidiMessage>> messages, bool rightSide)
                {
                    s->reset();
                    juce::AudioBuffer<float> buffer (2, 512);
                    te::MidiMessageArray midi;
                    std::vector<float> out;

                    for (int pos = 0; pos < total; pos += 512)
                    {
                        midi.clear();

                        for (auto& [at, m] : messages)
                            if (at >= pos && at < pos + 512)
                                midi.addMidiMessage (m, (at - pos) / sr, te::MPESourceID {});

                        buffer.clear();
                        te::PluginRenderContext rc (&buffer, 0, 512, &midi, 0.0, {}, true, false, false, false);
                        s->applyToBuffer (rc);

                        for (int i = 0; i < 512; ++i)
                            out.push_back (buffer.getSample (rightSide ? 1 : 0, i));
                    }

                    return out;
                };

                const auto on60 = juce::MidiMessage::noteOn (1, 60, 1.0f);
                defaults();
                o = renderMessages (4096, { { 0, on60 }, { 0, juce::MidiMessage::aftertouchChange (16, 60, 1) } }, true);
                check ("note pan -100 (sent after the note-on): right side silent", std::abs (o[1000]), 0.0, 1e-9);
                o = renderMessages (4096, { { 0, on60 }, { 0, juce::MidiMessage::aftertouchChange (16, 60, 1) } }, false);
                check ("note pan -100: left side untouched", o[1000], ramp (1000) - 1e-5, ramp (1000) + 1e-5);
                o = renderMessages (4096, { { 0, juce::MidiMessage::aftertouchChange (16, 60, 96) }, { 0, on60 } }, false);
                check ("note pan +51 (value 96): left side at 49%", o[1000] / ramp (1000), 0.48, 0.50);
                o = renderMessages (4096, { { 0, juce::MidiMessage::aftertouchChange (15, 60, 127) }, { 0, on60 } }, false);
                check ("note fine pitch +100 cents = one semitone", o[1000], ramp (1000 * std::pow (2.0, 1.0 / 12.0)) - 1e-4, ramp (1000 * std::pow (2.0, 1.0 / 12.0)) + 1e-4);
                o = renderMessages (4096, { { 0, juce::MidiMessage::aftertouchChange (16, 61, 1) }, { 0, on60 } }, true);
                check ("another key's pan doesn't touch this note", o[1000], ramp (1000) - 1e-5, ramp (1000) + 1e-5);
                o = renderMessages (4096, { { 0, juce::MidiMessage::aftertouchChange (16, 60, 1) }, { 2000, on60 } }, true);
                check ("a pan from an earlier moment doesn't stick to a later note", o[3000], ramp (1000) - 1e-5, ramp (1000) + 1e-5);
                defaults(); s->envOnParam->setParameter (1.0f, juce::sendNotificationSync); s->releaseParam->setParameter (100.0f, juce::sendNotificationSync);
                o = renderMessages (16384, { { 0, on60 }, { 4410, juce::MidiMessage::noteOff (1, 60, (juce::uint8) 32) } }, false);
                check ("release velocity 32 = half the release: half volume 25 ms after the note ends", o[4410 + 1102] / ramp (4410 + 1102), 0.48, 0.52);

                // Filters on the 5 kHz sine (level measured once it has settled).
                s->loadSample ("sine", dir.getChildFile ("sine.wav"));
                auto rmsDb = [&render, &hit]
                {
                    auto out = render (24576, hit);
                    double sum = 0.0;
                    for (int i = 10000; i < 20000; ++i) sum += out[(size_t) i] * out[(size_t) i];
                    return juce::Decibels::gainToDecibels (std::sqrt (sum / 10000.0), -200.0);
                };

                defaults();
                const double plain = rmsDb();
                check ("5 kHz sine, no filter (0.5 peak = -9 dB RMS)", plain, -9.1, -8.9);
                s->filterTypeParam->setParameter (1.0f, juce::sendNotificationSync); s->cutoffParam->setParameter (500.0f, juce::sendNotificationSync);
                check ("low pass 500 Hz: 5 kHz down by", rmsDb() - plain, -60.0, -30.0);
                s->filterTypeParam->setParameter (2.0f, juce::sendNotificationSync);
                check ("high pass 500 Hz: 5 kHz untouched", rmsDb() - plain, -0.3, 0.3);
                s->filterTypeParam->setParameter (3.0f, juce::sendNotificationSync); s->cutoffParam->setParameter (5000.0f, juce::sendNotificationSync);
                check ("band pass at 5 kHz: 0 dB at its centre", rmsDb() - plain, -0.5, 0.3);
                s->cutoffParam->setParameter (500.0f, juce::sendNotificationSync);
                check ("band pass at 500 Hz (soft, 6 dB/oct each side): 5 kHz down by", rmsDb() - plain, -16.0, -12.0);

                plugin = nullptr;
                edit = nullptr;
                return ok;
            },
            30000 });

        steps.push_back ({ "Piano roll in SONG mode: the playhead finds the viewed pattern's clip under the song position",
            [] {},
            [this]
            {
                auto edit = te::createEmptyEdit (engine, juce::File());
                auto a = song::addPattern (*edit, "A", 1);
                auto b = song::addPattern (*edit, "B", 2);
                song::addClip (*edit, a[song::ids::sid].toString(), 0, 8.0);
                song::addClip (*edit, b[song::ids::sid].toString(), 1, 0.0);
                song::addClip (*edit, a[song::ids::sid].toString(), 2, 16.0);

                const double inA = EditorState::beatInPattern (*edit, a, 9.5);
                const double gapA = EditorState::beatInPattern (*edit, a, 12.5);
                const double secondA = EditorState::beatInPattern (*edit, a, 19.75);
                const double inB = EditorState::beatInPattern (*edit, b, 6.0);
                const double before = EditorState::beatInPattern (*edit, a, 2.0);
                std::cout << "    A at 9.5: " << inA << ", A at 12.5: " << gapA << ", A at 19.75: " << secondA
                          << ", B at 6: " << inB << ", A at 2: " << before << std::endl;
                edit = nullptr;
                return std::abs (inA - 1.5) < 1e-9 && gapA < 0.0 && std::abs (secondA - 3.75) < 1e-9
                    && std::abs (inB - 6.0) < 1e-9 && before < 0.0;
            } });

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

        //==============================================================================
        // Parametric EQ (our own built-in effect)
        steps.push_back ({ "Parametric EQ: measured with test tones (peak +6 dB at 1 kHz, 48 dB/oct low cut, shelf, notch, main level, curve = sound)",
            [] {},
            []
            {
                const double sr = 48000.0;

                // Gain in dB of a sine at `hz` through the EQ (steady state).
                auto measure = [sr] (const std::array<peq::Band, peq::numBands>& bands, double mainDb, double hz)
                {
                    peq::Processor proc;
                    proc.prepare (sr);
                    proc.setTarget (bands, mainDb);
                    const int n = (int) sr;   // 1 s
                    juce::AudioBuffer<float> buf (2, n);

                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i)
                            buf.setSample (ch, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr));

                    proc.process (buf.getArrayOfWritePointers(), 2, n);
                    const int from = n / 2;   // skip the start
                    const float in = 0.25f / std::sqrt (2.0f);
                    return juce::Decibels::gainToDecibels (buf.getRMSLevel (0, from, n - from) / in, -200.0f);
                };

                auto flat = [] { std::array<peq::Band, peq::numBands> b; for (int i = 0; i < peq::numBands; ++i) b[(size_t) i] = ParametricEQPlugin::defaultBand (i); return b; };
                bool ok = true;
                auto check = [&ok] (const char* what, double got, double lo, double hi)
                {
                    const bool good = got >= lo && got <= hi;
                    std::cout << "    " << what << ": " << juce::String (got, 2) << " dB" << (good ? "" : "   <-- expected " + juce::String (lo) + " .. " + juce::String (hi)) << std::endl;
                    ok = ok && good;
                };

                auto bands = flat();
                check ("flat EQ at 1 kHz", measure (bands, 0.0, 1000.0), -0.05, 0.05);

                bands[3] = { peq::peak, 0, 1000.0, 6.0, 1.0 };
                check ("peak +6 dB @ 1 kHz, at 1 kHz", measure (bands, 0.0, 1000.0), 5.8, 6.2);
                check ("same peak, at 100 Hz", measure (bands, 0.0, 100.0), -0.1, 0.3);
                check ("curve drawn at 1 kHz", peq::responseDb (bands, 0.0, 1000.0, sr), 5.95, 6.05);

                bands = flat();
                bands[0] = { peq::highPass, 3, 1000.0, 0.0, 0.707 };
                check ("48 dB/oct low cut @ 1 kHz, at 250 Hz", measure (bands, 0.0, 250.0), -200.0, -60.0);
                check ("same low cut, at 8 kHz", measure (bands, 0.0, 8000.0), -0.2, 0.2);
                check ("same low cut, at the cut-off (Butterworth: -3 dB)", measure (bands, 0.0, 1000.0), -3.3, -2.7);

                bands = flat();
                bands[6] = { peq::highShelf, 0, 4000.0, -6.0, 0.707 };
                check ("high shelf -6 dB @ 4 kHz, at 16 kHz", measure (bands, 0.0, 16000.0), -6.3, -5.5);
                check ("same shelf, at 200 Hz", measure (bands, 0.0, 200.0), -0.1, 0.1);

                bands = flat();
                bands[2] = { peq::notch, 0, 500.0, 0.0, 4.0 };
                check ("notch @ 500 Hz, at 500 Hz", measure (bands, 0.0, 500.0), -200.0, -30.0);

                check ("main level -6 dB", measure (flat(), -6.0, 1000.0), -6.05, -5.95);
                return ok;
            } });

        steps.push_back ({ "Parametric EQ: host puts one on the master, peer 1 boosts band 4 (+6 dB @ 1.2 kHz) and makes band 1 a 48 dB/oct low cut at 80 Hz -> the same on every PC",
            [this]
            {
                song::putEffect (H(), nullptr, song::createEffect (H(), ParametricEQPlugin::xmlTypeName, nullptr), -1);
            },
            [this]
            {
                auto findEq = [] (te::Edit& e) -> ParametricEQPlugin*
                {
                    for (auto* p : e.getMasterPluginList())
                        if (auto* eq = dynamic_cast<ParametricEQPlugin*> (p))
                            return eq;
                    return nullptr;
                };

                // Peer 1 edits once its copy has the EQ.
                if (auto* eq = findEq (P1()); eq != nullptr && ! eqEdited)
                {
                    P1().getUndoManager().beginNewTransaction();
                    eq->setBand (3, { peq::peak, 0, 1200.0, 6.0, 1.0 });
                    eq->setBand (0, { peq::highPass, 3, 80.0, 0.0, 0.707 });
                    eqEdited = true;
                    return false;
                }

                return eqEdited && hashesEqual() && everywhere ([&findEq] (te::Edit& e)
                {
                    auto* eq = findEq (e);

                    if (eq == nullptr)
                        return false;

                    auto b = eq->getBands();
                    return b[3].type == peq::peak && std::abs (b[3].gain - 6.0) < 1.0e-3 && std::abs (b[3].freq - 1200.0) < 0.5
                        && b[0].type == peq::highPass && b[0].order == 3 && std::abs (b[0].freq - 80.0) < 0.5;
                });
            },
            8000, 600 });

        //==============================================================================
        // Limiter (our own built-in effect)
        steps.push_back ({ "Limiter: measured (brickwall ceiling, look-ahead delay, compressor 4:1, gate, saturation)",
            [] {},
            []
            {
                const double sr = 48000.0;
                bool ok = true;
                auto check = [&ok] (const juce::String& what, double got, double lo, double hi)
                {
                    const bool good = got >= lo && got <= hi;
                    std::cout << "    " << what << ": " << juce::String (got, 3) << (good ? "" : "   <-- expected " + juce::String (lo) + " .. " + juce::String (hi)) << std::endl;
                    ok = ok && good;
                };

                // Runs a signal through a limiter with these settings (look-ahead in samples).
                juce::int64 clamps = 0;
                auto run = [sr, &clamps] (const lim::Settings& s, int lookahead, std::function<float (int, int)> signal, int n)
                {
                    lim::Processor proc;
                    proc.prepare (sr, 512);
                    proc.setSettings (s);
                    proc.setLookaheadSamples (lookahead);
                    juce::AudioBuffer<float> buf (2, n);

                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i)
                            buf.setSample (ch, i, signal (ch, i));

                    for (int pos = 0; pos < n; pos += 512)   // in blocks, like the engine
                    {
                        float* chans[2] = { buf.getWritePointer (0, pos), buf.getWritePointer (1, pos) };
                        proc.process (chans, 2, juce::jmin (512, n - pos));
                    }

                    clamps = proc.safetyClamps;
                    return buf;
                };

                auto sine = [sr] (double hz, double amp) { return [=] (int, int i) { return (float) (amp * std::sin (juce::MathConstants<double>::twoPi * hz * i / sr)); }; };
                auto dbOf = [] (float v) { return (double) juce::Decibels::gainToDecibels (v, -200.0f); };
                const int n = (int) sr;

                // 1. Brickwall: +6 dBFS sine plus single-sample spikes of +12 dBFS; ceiling -1 dB, 2 ms look-ahead.
                lim::Settings s;
                s.ceilingDb = -1.0;
                s.limReleaseMs = 50.0;
                auto spiky = [sr] (int ch, int i)
                {
                    double v = 2.0 * std::sin (juce::MathConstants<double>::twoPi * 997.0 * i / sr);
                    if (i % 4801 == 100 + ch) v = (i / 4801) % 2 == 0 ? 4.0 : -4.0;
                    return (float) v;
                };
                auto out = run (s, 96, spiky, n);
                check ("brickwall: loudest output sample (dBFS, ceiling -1)", dbOf (out.getMagnitude (0, n)), -200.0, -1.0 + 1.0e-4);
                check ("brickwall: steady sine level after limiting (dBFS peak)", dbOf (out.getMagnitude (0, n / 2, n / 2 - 2000)), -1.6, -1.0 + 1.0e-4);
                check ("brickwall: samples the safety clamp had to catch (want 0)", (double) clamps, 0.0, 0.0);

                // 2. Look-ahead: a quiet click comes out exactly 96 samples later.
                lim::Settings quiet;
                auto click = run (quiet, 96, [] (int, int i) { return i == 1000 ? 0.1f : 0.0f; }, 4096);
                int at = 0;
                for (int i = 0; i < 4096; ++i) if (std::abs (click.getSample (0, i)) > 0.05f) at = i;
                check ("look-ahead: click comes out at sample (want 1096)", at, 1096, 1096);

                // 3. Compressor: -10 dB RMS in, threshold -20 dB, 4:1, hard knee -> -17.5 dB RMS out.
                lim::Settings c;
                c.compThreshDb = -20.0;  c.compRatio = 4.0;  c.compKneeDb = 0.0;
                c.compAttackMs = 1.0;    c.compReleaseMs = 50.0;  c.compSustainMs = 50.0;
                c.ceilingDb = 12.0;   // limiter out of the way
                const double amp = std::sqrt (2.0) * juce::Decibels::decibelsToGain (-10.0);
                auto comp = run (c, 0, sine (1000.0, amp), n);
                check ("compressor: output RMS (dB, want -17.5)", dbOf (comp.getRMSLevel (0, n / 2, n / 2)), -18.0, -17.0);

                // 4. Gate: closes on -60 dB, passes -20 dB.
                lim::Settings gt;
                gt.gateGain = 0.0;  gt.gateThreshDb = -40.0;  gt.gateReleaseMs = 20.0;  gt.ceilingDb = 12.0;
                auto low = run (gt, 0, sine (440.0, juce::Decibels::decibelsToGain (-60.0)), n);
                auto high = run (gt, 0, sine (440.0, juce::Decibels::decibelsToGain (-20.0)), n);
                check ("gate: -60 dB in -> out (dB)", dbOf (low.getMagnitude (0, n / 2, n / 2)), -200.0, -100.0);
                check ("gate: -20 dB in -> out (dB)", dbOf (high.getMagnitude (0, n / 2, n / 2)), -20.1, -19.9);

                // 5. Saturation (12 dB below a 0 dB ceiling): never over, quiet audio untouched.
                lim::Settings st;
                st.satDb = 12.0;  st.limReleaseMs = 50.0;
                auto loud = run (st, 0, sine (200.0, 2.0), n);
                auto soft = run (st, 0, sine (200.0, juce::Decibels::decibelsToGain (-30.0)), n);
                check ("saturation: loudest sample (dBFS)", dbOf (loud.getMagnitude (0, n)), -200.0, 1.0e-4);
                check ("saturation: -30 dB passes (dB)", dbOf (soft.getMagnitude (0, n / 2, n / 2)), -30.05, -29.95);
                return ok;
            } });

        steps.push_back ({ "Limiter: host puts one on the master, peer 2 sets the ceiling to -3 dB and look-ahead to 5 ms -> same everywhere, latency reported",
            [this]
            {
                song::putEffect (H(), nullptr, song::createEffect (H(), LimiterPlugin::xmlTypeName, nullptr), -1);
            },
            [this]
            {
                auto find = [] (te::Edit& e) -> LimiterPlugin*
                {
                    for (auto* p : e.getMasterPluginList())
                        if (auto* l = dynamic_cast<LimiterPlugin*> (p))
                            return l;
                    return nullptr;
                };

                if (auto* l = find (P2()); l != nullptr && ! limiterEdited)
                {
                    P2().getUndoManager().beginNewTransaction();
                    l->ceiling->setParameter (-3.0f, juce::sendNotification);
                    l->limAttack->setParameter (5.0f, juce::sendNotification);
                    limiterEdited = true;
                    return false;
                }

                return limiterEdited && hashesEqual() && everywhere ([&find] (te::Edit& e)
                {
                    auto* l = find (e);
                    return l != nullptr && std::abs (l->ceiling->getCurrentValue() + 3.0f) < 1.0e-4f
                        && std::abs (l->limAttack->getCurrentValue() - 5.0f) < 1.0e-4f
                        && std::abs (l->getLatencySeconds() - 0.005) < 0.0002;   // after the knob settles
                });
            },
            8000, 1200 });

        //==============================================================================
        // Delay (our own built-in effect)
        steps.push_back ({ "Delay: measured (echo times and levels, ping pong, tempo sync, offset, feedback filter, 120% feedback stays bounded, model off)",
            [] {},
            []
            {
                const double sr = 48000.0;
                bool ok = true;
                auto check = [&ok] (const juce::String& what, double got, double lo, double hi)
                {
                    const bool good = got >= lo && got <= hi && ! std::isnan (got);
                    std::cout << "    " << what << ": " << juce::String (got, 4) << (good ? "" : "   <-- expected " + juce::String (lo) + " .. " + juce::String (hi)) << std::endl;
                    ok = ok && good;
                };

                auto run = [sr] (const dly::Settings& s, std::function<float (int, int)> signal, int n)
                {
                    dly::Processor proc;
                    proc.prepare (sr);
                    proc.setSettings (s);
                    juce::AudioBuffer<float> buf (2, n);

                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i)
                            buf.setSample (ch, i, signal (ch, i));

                    for (int pos = 0; pos < n; pos += 480)
                    {
                        float* chans[2] = { buf.getWritePointer (0, pos), buf.getWritePointer (1, pos) };
                        proc.process (chans, 2, juce::jmin (480, n - pos));
                    }

                    return buf;
                };

                // Plain echoes: wet only, no filter, limiter out of the way.
                dly::Settings base;
                base.tempoSync = false;  base.timeMs = 100.0;  base.feedback = 0.5;
                base.filterType = dly::filterOff;  base.wet = 1.0;  base.dry = 0.0;  base.offset = 0.0;
                auto click = [] (int, int i) { return i == 1000 ? 0.5f : 0.0f; };
                auto peakAt = [] (const juce::AudioBuffer<float>& b, int ch, int at) { return (double) b.getSample (ch, at); };
                auto loudestOutside = [] (const juce::AudioBuffer<float>& b, int ch, std::initializer_list<int> expected)
                {
                    float m = 0.0f;
                    for (int i = 0; i < b.getNumSamples(); ++i)
                    {
                        bool near = false;
                        for (auto e : expected) near = near || std::abs (i - e) <= 2;
                        if (! near) m = juce::jmax (m, std::abs (b.getSample (ch, i)));
                    }
                    return (double) m;
                };

                auto st = run (base, click, 20000);
                check ("stereo: 1st echo at 100 ms (level)", peakAt (st, 0, 5800), 0.495, 0.505);
                check ("stereo: 2nd echo at 200 ms", peakAt (st, 0, 10600), 0.245, 0.255);
                check ("stereo: 3rd echo at 300 ms", peakAt (st, 1, 15400), 0.12, 0.13);
                check ("stereo: anything between the echoes", loudestOutside (st, 0, { 5800, 10600, 15400 }), 0.0, 1.0e-4);

                // Ping pong: a click on the left only -> L, R, L.
                auto pp = base;
                pp.model = dly::pingPong;
                auto ping = run (pp, [] (int ch, int i) { return ch == 0 && i == 1000 ? 0.5f : 0.0f; }, 20000);
                check ("ping pong: 1st echo left", peakAt (ping, 0, 5800), 0.495, 0.505);
                check ("ping pong: 1st echo not on the right", std::abs (peakAt (ping, 1, 5800)), 0.0, 1.0e-4);
                check ("ping pong: 2nd echo right", peakAt (ping, 1, 10600), 0.245, 0.255);
                check ("ping pong: 3rd echo left", peakAt (ping, 0, 15400), 0.12, 0.13);

                // Tempo sync: a 1/4 note at 120 BPM = 0.5 s.
                auto ts = base;
                ts.tempoSync = true;  ts.division = 10;  ts.bpm = 120.0;  ts.feedback = 0.0;
                auto sync = run (ts, click, 30000);
                check ("tempo sync: 1/4 at 120 BPM lands at sample 25000", peakAt (sync, 0, 25000), 0.495, 0.505);

                // Offset +50% (stereo): the right side's echoes come half an echo later.
                auto of = base;
                of.offset = 0.5;  of.feedback = 0.0;
                auto offs = run (of, click, 20000);
                check ("offset: left echo at 100 ms", peakAt (offs, 0, 5800), 0.495, 0.505);
                check ("offset: right echo at 150 ms", peakAt (offs, 1, 8200), 0.495, 0.505);

                // Feedback filter (low pass 1 kHz): 200 Hz passes, 8 kHz is cut about 36 dB.
                auto lp = base;
                lp.feedback = 0.0;  lp.filterType = dly::lowPass;  lp.cutoffHz = 1000.0;
                auto tone = [sr] (double hz) { return [=] (int, int i) { return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * hz * i / sr)); }; };
                auto low = run (lp, tone (200.0), 48000), high = run (lp, tone (8000.0), 48000);
                const float inRms = 0.5f / std::sqrt (2.0f);
                check ("filter LP 1 kHz: 200 Hz (dB)", juce::Decibels::gainToDecibels (low.getRMSLevel (0, 24000, 24000) / inRms), -0.5, 0.1);
                check ("filter LP 1 kHz: 8 kHz (dB)", juce::Decibels::gainToDecibels (high.getRMSLevel (0, 24000, 24000) / inRms), -45.0, -30.0);

                // 120% feedback with the limit at 0 dB: grows but stays bounded, never NaN.
                auto hot = base;
                hot.feedback = 1.2;  hot.distType = dly::limit;  hot.distLevelDb = 0.0;
                juce::Random rnd (7);
                auto noise = [&rnd] (int, int i) { return i < 4800 ? rnd.nextFloat() - 0.5f : 0.0f; };
                auto runaway = run (hot, noise, 48000 * 4);
                const double hotPeak = juce::jmax (runaway.getMagnitude (0, 0, runaway.getNumSamples()), runaway.getMagnitude (1, 0, runaway.getNumSamples()));
                check ("120% feedback: loudest sample after 4 s (bounded by the limit)", hotPeak, 0.2, 1.0 + 1.0e-6);

                // Model off, filter off: the audio passes untouched.
                auto offModel = base;
                offModel.model = dly::off;
                auto passed = run (offModel, tone (440.0), 9600);
                double maxDiff = 0.0;
                for (int i = 0; i < 9600; ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (passed.getSample (0, i) - tone (440.0) (0, i)));
                check ("model off: difference from the input", maxDiff, 0.0, 1.0e-6);
                return ok;
            } });

        steps.push_back ({ "Delay: host puts one on the master, peer 1 sets ping pong and 65% feedback -> same everywhere",
            [this]
            {
                song::putEffect (H(), nullptr, song::createEffect (H(), DelayPlugin::xmlTypeName, nullptr), -1);
            },
            [this]
            {
                auto find = [] (te::Edit& e) -> DelayPlugin*
                {
                    for (auto* p : e.getMasterPluginList())
                        if (auto* d = dynamic_cast<DelayPlugin*> (p))
                            return d;
                    return nullptr;
                };

                if (auto* d = find (P1()); d != nullptr && ! delayEdited)
                {
                    P1().getUndoManager().beginNewTransaction();
                    d->model->setParameter ((float) dly::pingPong, juce::sendNotification);
                    d->feedback->setParameter (0.65f, juce::sendNotification);
                    delayEdited = true;
                    return false;
                }

                return delayEdited && hashesEqual() && everywhere ([&find] (te::Edit& e)
                {
                    auto* d = find (e);
                    return d != nullptr && d->getSettings().model == dly::pingPong
                        && std::abs (d->feedback->getCurrentValue() - 0.65f) < 1.0e-4f;
                });
            },
            8000, 600 });

        //==============================================================================
        // Reverb (our own built-in effect)
        steps.push_back ({ "Reverb: measured (RT60 = the decay knob, bass multiplier, damping, pre-delay, dry / side exact, 20 s decay stays bounded)",
            [] {},
            []
            {
                const double sr = 48000.0;
                bool ok = true;
                auto check = [&ok] (const juce::String& what, double got, double lo, double hi)
                {
                    const bool good = got >= lo && got <= hi && ! std::isnan (got);
                    std::cout << "    " << what << ": " << juce::String (got, 3) << (good ? "" : "   <-- expected " + juce::String (lo) + " .. " + juce::String (hi)) << std::endl;
                    ok = ok && good;
                };

                auto run = [sr] (const rvb::Settings& s, std::function<float (int, int)> signal, int n)
                {
                    rvb::Processor proc;
                    proc.prepare (sr);
                    proc.setSettings (s);
                    juce::AudioBuffer<float> buf (2, n);

                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i)
                            buf.setSample (ch, i, signal (ch, i));

                    for (int pos = 0; pos < n; pos += 512)
                    {
                        float* chans[2] = { buf.getWritePointer (0, pos), buf.getWritePointer (1, pos) };
                        proc.process (chans, 2, juce::jmin (512, n - pos));
                    }

                    return buf;
                };

                // RT60 from an impulse response: Schroeder backward integration, T30 (-5 to -35 dB) x 2.
                // `band`: 0 = whole, 1 = lows (2 x one-pole low pass at 150 Hz), 2 = highs (2 x high pass at 5 kHz).
                auto rt60 = [sr] (const juce::AudioBuffer<float>& ir, int band)
                {
                    const int n = ir.getNumSamples();
                    std::vector<double> e ((size_t) n);
                    const double c150 = rvb::onePoleCoef (150.0, sr), c5k = rvb::onePoleCoef (5000.0, sr);
                    double a = 0, b = 0;

                    for (int i = 0; i < n; ++i)
                    {
                        double x = ir.getSample (0, i);

                        if (band == 1) { a += (x - a) * c150; b += (a - b) * c150; x = b; }
                        if (band == 2) { a += (x - a) * c5k; x -= a; b += (x - b) * c5k; x -= b; }

                        e[(size_t) i] = x * x;
                    }

                    for (int i = n - 2; i >= 0; --i)
                        e[(size_t) i] += e[(size_t) i + 1];

                    const double total = e[0];
                    double t5 = -1.0, t35 = -1.0;

                    for (int i = 0; i < n; ++i)
                    {
                        const double db = 10.0 * std::log10 (juce::jmax (1.0e-30, e[(size_t) i] / total));
                        if (t5 < 0.0 && db <= -5.0)  t5 = i / sr;
                        if (t35 < 0.0 && db <= -35.0) { t35 = i / sr; break; }
                    }

                    return t5 >= 0.0 && t35 > t5 ? 2.0 * (t35 - t5) : -1.0;
                };

                rvb::Settings late;   // only the late reverb
                late.dry = 0.0;  late.early = 0.0;  late.wet = 1.0;  late.predelayMs = 0.0;
                late.lowCutHz = 10.0;  late.highCutHz = 20000.0;  late.dampHz = 20000.0;
                late.modDepth = 0.0;  late.bassMultiplier = 1.0;
                auto impulse = [] (int, int i) { return i == 0 ? 1.0f : 0.0f; };

                for (double rt : { 0.5, 2.0, 6.0 })
                {
                    auto s = late;
                    s.decaySeconds = rt;
                    auto ir = run (s, impulse, (int) (sr * (rt * 1.2 + 0.5)));
                    check ("RT60 measured, decay " + juce::String (rt, 1) + " s", rt60 (ir, 0), rt * 0.9, rt * 1.1);
                }

                {
                    auto s = late;
                    s.decaySeconds = 2.0;  s.modDepth = 0.2;  s.modSpeedHz = 0.6;   // the default modulation
                    auto ir = run (s, impulse, (int) (sr * 2.9));
                    check ("RT60 measured, decay 2.0 s with modulation", rt60 (ir, 0), 1.8, 2.2);
                }

                {
                    auto s = late;
                    s.decaySeconds = 1.0;  s.bassMultiplier = 3.0;  s.crossoverHz = 300.0;
                    auto ir = run (s, impulse, (int) (sr * 4.5));
                    const double lows = rt60 (ir, 1), highs = rt60 (ir, 2);
                    check ("bass x3: lows last this many times longer than highs", lows / juce::jmax (1.0e-6, highs), 2.0, 4.0);
                }

                {
                    auto s = late;
                    s.decaySeconds = 2.0;  s.dampHz = 2000.0;
                    auto ir = run (s, impulse, (int) (sr * 3.0));
                    const double lows = rt60 (ir, 1), highs = rt60 (ir, 2);
                    check ("damping 2 kHz: highs / lows decay time", highs / juce::jmax (1.0e-6, lows), 0.0, 0.6);
                }

                {
                    auto s = late;
                    s.early = 1.0;  s.wet = 0.0;  s.predelayMs = 100.0;
                    auto ir = run (s, [] (int, int i) { return i == 0 ? 1.0f : 0.0f; }, 24000);
                    int first = -1;
                    for (int i = 0; i < 24000 && first < 0; ++i) if (std::abs (ir.getSample (0, i)) > 1.0e-6f) first = i;
                    check ("pre-delay 100 ms: first sound at sample", first, 4800, 4800 + 0.08 * sr);
                }

                auto tone = [sr] (int ch, int i) { return (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * (ch == 0 ? 440.0 : 550.0) * i / sr)); };
                auto maxDiff = [] (const juce::AudioBuffer<float>& b, std::function<float (int, int)> f)
                {
                    double m = 0.0;
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < b.getNumSamples(); ++i)
                            m = juce::jmax (m, (double) std::abs (b.getSample (ch, i) - f (ch, i)));
                    return m;
                };

                {
                    auto s = late;
                    s.dry = 1.0;  s.wet = 0.0;  s.early = 0.0;
                    check ("dry only: difference from the input", maxDiff (run (s, tone, 9600), tone), 0.0, 1.0e-7);
                }

                {
                    auto s = late;
                    s.dry = 1.0;  s.wet = 1.0;  s.early = 1.0;  s.side = true;
                    auto mono = [sr] (int, int i) { return (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sr)); };
                    check ("side mode, mono input: no reverb added", maxDiff (run (s, mono, 9600), mono), 0.0, 1.0e-7);
                }

                {
                    auto s = late;
                    s.decaySeconds = 20.0;  s.bassMultiplier = 4.0;  s.modDepth = 1.0;  s.size = 1.0;
                    juce::Random rnd (3);
                    auto noise = [&rnd] (int, int i) { return i < 96000 ? 0.25f * (rnd.nextFloat() * 2.0f - 1.0f) : 0.0f; };
                    auto out = run (s, noise, 48000 * 10);
                    const double peak = juce::jmax (out.getMagnitude (0, 0, out.getNumSamples()), out.getMagnitude (1, 0, out.getNumSamples()));
                    const double during = out.getRMSLevel (0, 48000, 48000), end = out.getRMSLevel (0, 9 * 48000, 48000);
                    check ("20 s decay, bass x4: loudest sample (bounded)", peak, 0.0, 2.0);
                    check ("20 s decay, bass x4: last second vs noise second (dB, decays)", juce::Decibels::gainToDecibels (end / juce::jmax (1.0e-9, during)), -60.0, -6.0);
                }

                return ok;
            } });

        steps.push_back ({ "Reverb: host puts one on the master, peer 2 sets a 4 s decay and 40% wet -> same everywhere",
            [this]
            {
                song::putEffect (H(), nullptr, song::createEffect (H(), ReverbPlugin::xmlTypeName, nullptr), -1);
            },
            [this]
            {
                auto find = [] (te::Edit& e) -> ReverbPlugin*
                {
                    for (auto* p : e.getMasterPluginList())
                        if (auto* r = dynamic_cast<ReverbPlugin*> (p))
                            return r;
                    return nullptr;
                };

                if (auto* r = find (P2()); r != nullptr && ! reverbEdited)
                {
                    P2().getUndoManager().beginNewTransaction();
                    r->decay->setParameter (4.0f, juce::sendNotification);
                    r->wet->setParameter (0.4f, juce::sendNotification);
                    reverbEdited = true;
                    return false;
                }

                return reverbEdited && hashesEqual() && everywhere ([&find] (te::Edit& e)
                {
                    auto* r = find (e);
                    return r != nullptr && std::abs (r->decay->getCurrentValue() - 4.0f) < 1.0e-4f
                        && std::abs (r->wet->getCurrentValue() - 0.4f) < 1.0e-4f;
                });
            },
            8000, 600 });

        steps.push_back ({ "Mixer: the master and an insert take all 10 effect slots (the insert with 6 sends too) -> on every PC",
            [this]
            {
                // The engine's own default allowed only 4 plugins on the master and 16 on a track.
                while (song::getMasterEffects (H()).size() < 10)
                    song::putEffect (H(), nullptr, song::createEffect (H(), ParametricEQPlugin::xmlTypeName, nullptr), -1);

                auto* full = song::addInsert (H(), "Full");

                for (int i = 0; i < 6; ++i)
                    if (auto* target = song::addInsert (H(), "Send " + juce::String (i + 1)))
                        song::addSend (H(), full->itemID.toString(), target->itemID.toString(), -6.0);

                for (int i = 0; i < 10; ++i)
                    song::putEffect (H(), full, song::createEffect (H(), ParametricEQPlugin::xmlTypeName, nullptr), -1);
            },
            [this]
            {
                return hashesEqual() && everywhere ([] (te::Edit& e)
                {
                    auto* full = insert (e, "Full");
                    return full != nullptr && song::getMasterEffects (e).size() == 10
                        && song::getInsertEffects (*full).size() == 10
                        && song::getSendsFrom (e, full->itemID.toString()).size() == 6;
                });
            },
            8000, 600 });

        steps.push_back ({ "Cache clean-up: audio of a song closed without saving leaves this PC's cache; audio the saved project keeps, or the next song uses, stays",
            [this]
            {
                fourth.newProject();
                auto folder = testRoot.getChildFile ("fourth-cleanup-assets");
                folder.deleteRecursively();
                fourth.setAssetFolder (folder);
                auto& store = fourth.getAssetStore();
                auto* e = fourth.getEdit();

                // Two files come in: one gets saved in the project, the other never does.
                const auto unsavedTake = store.importAudioFile (toneFile);
                const auto savedTake = store.importAudioFile (voiceFile);
                song::addImportedAudio (*e, unsavedTake, "tone.wav", 0, 0.0);
                song::addImportedAudio (*e, savedTake, "voice.wav", 1, 0.0);

                const auto projectFile = testRoot.getChildFile ("cleanup").getChildFile ("cleanup.cotosong");
                std::vector<project::AudioFile> audio { { savedTake.hash, "voice.wav", false } };
                const auto saved = project::save (projectFile, e->state, store, audio, true, "test");
                fourth.setSongFile (projectFile);

                const auto unsaved = fourth.getUnsavedAudio();
                fourth.forgetUnsavedAudio();
                fourth.newProject();   // closes it ("don't save")

                const bool firstOK = saved.ok() && unsaved.contains (unsavedTake.hash) && ! unsaved.contains (savedTake.hash)
                                      && ! store.has (unsavedTake.hash) && store.has (savedTake.hash)
                                      && projectFile.getSiblingFile ("Audio").getChildFile ("voice.flac").existsAsFile()
                                      && store.getAddedSinceMark().isEmpty();

                // A file the next song also uses stays.
                const auto shared = store.importAudioFile (writeTestWav (testRoot.getChildFile ("shared.wav"), 0.5, 330.0, false));
                song::addImportedAudio (*fourth.getEdit(), shared, "shared.wav", 0, 0.0);
                fourth.setSongFile ({});
                fourth.forgetUnsavedAudio();
                fourth.newProject ([shared] (te::Edit& ed) { song::addImportedAudio (ed, shared, "shared.wav", 0, 0.0); }, "next");
                const bool secondOK = store.has (shared.hash);

                std::cout << "    unsaved file gone: " << ! store.has (unsavedTake.hash) << ", saved one kept: " << store.has (savedTake.hash)
                          << ", used by the next song kept: " << secondOK << std::endl;
                cleanupCheck = firstOK && secondOK;
            },
            [this] { return cleanupCheck; } });

        steps.push_back ({ "Delete unused takes: finds the takes with no clip (not imported files, not used ones)",
            [] {},
            [this]
            {
                auto edit = te::createEmptyEdit (engine, juce::File());
                auto* insert = song::addInsert (*edit, "Vocals");

                auto result = [] (const char* hash)
                {
                    AssetStore::ImportResult r;
                    r.hash = hash;
                    r.lengthSeconds = 1.0;
                    r.sampleRate = 44100.0;
                    r.numChannels = 1;
                    return r;
                };

                const auto kept = result ("11111111111111111111111111111111"), unused = result ("22222222222222222222222222222222");
                song::addRecordedTake (*edit, kept, insert->itemID.toString(), 0.0, 0.0, 1.0);
                auto clip = song::addRecordedTake (*edit, unused, insert->itemID.toString(), 4.0, 0.0, 1.0);
                song::removeClip (*edit, clip);
                song::addAsset (*edit, result ("33333333333333333333333333333333"), "imported.wav");   // not a take

                const auto found = song::findUnusedTakes (*edit);
                std::cout << "    found: " << found.joinIntoString (", ") << std::endl;
                edit = nullptr;
                return found.size() == 1 && found[0] == unused.hash;
            } });

        //==============================================================================
        // 1.8: cutting and fading clips, time-stretch and pitch
        steps.push_back ({ "1.8 clips: host imports a 2 s tone onto track 15 at beat 400 -> everyone has it",
            [this]
            {
                for (auto* sess : { &host, &peer1, &peer2 })
                    sess->setSongMode (true);

                auto r = host.getAssetStore().importAudioFile (writeTestWav (testRoot.getChildFile ("cuts.wav"), 2.0, 330.0, false));
                cutsHash = r.hash;

                if (r.ok())
                    song::addImportedAudio (H(), r, "cuts.wav", 14, 400.0);
            },
            [this]
            {
                if (cutsHash.isEmpty() || ! everywhere ([this] (te::Edit& e) { return audioClipOf (e, cutsHash).isValid(); })
                     || ! hasVerified (peer1, cutsHash) || ! hasVerified (peer2, cutsHash) || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                // Its level as imported (to compare the clip volume with).
                if (! audioReady (H(), { "cuts" }))
                    return false;

                const double start = song::beatsToSeconds (H(), 400.0);
                cutsBaseline = peakIn (renderTracks (H(), { "cuts" }, start, start + 2.0), 0.5, 0.9);
                std::cout << "    level as imported: " << cutsBaseline << std::endl;
                return cutsBaseline > 0.1f;
            },
            15000 });

        steps.push_back ({ "1.8 clips: host fades it (0.25 s in, 0.5 s out), turns it up 6 dB and cuts it at 1 s -> two parts everywhere, it sounds the same",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                auto clip = audioClipOf (H(), cutsHash);
                song::setClipFades (H(), clip, 0.25, 0.5);
                clip.setProperty (song::ids::gain, 6.0, &H().getUndoManager());
                const double cutBeat = song::secondsToBeats (H(), song::beatsToSeconds (H(), 400.0) + 1.0);
                auto right = song::splitClip (H(), clip, cutBeat);
                std::cout << "    cut: " << (right.isValid() ? "yes" : "NO") << std::endl;
            },
            [this]
            {
                const bool structure = everywhere ([this] (te::Edit& e)
                {
                    int parts = 0;

                    for (auto& c : song::getClips (e))
                        if (song::isAudioClip (c) && c[song::ids::asset].toString() == cutsHash)
                            ++parts;

                    return parts == 2;
                });

                if (! structure || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                const auto engine = compiledAudio (host, "cuts");
                std::cout << "    engine: " << engine.joinIntoString (" | ") << std::endl;

                if (engine.size() != 2 || ! engine[0].contains ("+1.000>0.000 fi=0.250 g=6.00") || engine[0].contains ("fo=")
                     || ! engine[1].contains ("+1.000>1.000 fo=0.500 g=6.00") || engine[1].contains ("fi="))
                    return false;

                if (! audioReady (H(), { "cuts" }))
                    return false;

                const double start = song::beatsToSeconds (H(), 400.0);
                const auto b = renderTracks (H(), { "cuts" }, start, start + 2.0);
                const float atStart = peakIn (b, 0.0, 0.02), middle = peakIn (b, 0.5, 1.4), atEnd = peakIn (b, 1.97, 2.0);
                std::cout << "    level: start " << atStart << ", middle " << middle << " (x" << middle / cutsBaseline
                          << "), end " << atEnd << std::endl;
                return atStart < 0.1f * middle && atEnd < 0.1f * middle
                    && middle / cutsBaseline > 1.9f && middle / cutsBaseline < 2.1f;
            },
            8000, 600 });

        steps.push_back ({ "1.8 clips: peer 1 cuts a pattern clip in two, then trims the right part's start -> the song plays the same notes, minus the trimmed one",
            [this]
            {
                auto cut = song::addPattern (P1(), "Cut", 1);

                for (int step : { 0, 4, 8, 12 })
                    song::setStep (P1(), cut, *ch (P1(), "Kick"), step, true);

                song::addClip (P1(), cut[song::ids::sid].toString(), 15, 408.0);
            },
            [this]
            {
                if (! everywhere ([] (te::Edit& e) { return pat (e, "Cut").isValid(); }) || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (auto* c = peer1.getCompiler())
                    c->compileNow();

                compiledBeforeCut = compiled (peer1, "Kick");
                juce::ValueTree clip;

                for (auto& c : song::getClips (P1()))
                    if (c[song::ids::pattern].toString() == pat (P1(), "Cut")[song::ids::sid].toString())
                        clip = c;

                P1().getUndoManager().beginNewTransaction();
                auto right = song::splitClip (P1(), clip, 410.0);

                if (auto* c = peer1.getCompiler())
                    c->compileNow();

                const bool same = compiled (peer1, "Kick") == compiledBeforeCut;
                std::cout << "    after the cut: " << (right.isValid() ? "2 clips" : "NOT CUT") << ", same notes: " << same
                          << ", right part starts " << (double) right[song::ids::poff] << " beats into the pattern" << std::endl;

                // Trim the right part's start by one beat (like dragging its left edge).
                right.setProperty (song::ids::start, 411.0, &P1().getUndoManager());
                right.setProperty (song::ids::poff, 3.0, &P1().getUndoManager());
                right.setProperty (song::ids::length, 1.0, &P1().getUndoManager());
                return same && right.isValid() && std::abs ((double) right[song::ids::poff] - 3.0) < 1.0e-9;
            },
            8000, 600 });

        steps.push_back ({ "1.8 clips: ...everyone plays the kick at 408, 409 and 411 (not 410)",
            {},
            [this]
            {
                if (! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                const auto notes = compiled (host, "Kick").joinIntoString (" ");
                return notes.contains ("@408.000") && notes.contains ("@409.000") && notes.contains ("@411.000") && ! notes.contains ("@410.000");
            },
            8000, 600 });

        steps.push_back ({ "1.8 stretch: peer 2 pitches the right part +12 (and turns the left part down) -> an octave higher (660 Hz), exactly in place and as long as before",
            [this]
            {
                for (auto& c : song::getClips (P2()))
                    if (song::isAudioClip (c) && c[song::ids::asset].toString() == cutsHash)
                    {
                        if ((double) c[song::ids::offset] > 0.5)
                        {
                            c.setProperty (song::ids::pitch, 12.0, &P2().getUndoManager());
                            song::setClipFades (P2(), c, 0.0, 0.0);   // to see exactly where it starts and ends
                        }
                        else
                        {
                            c.setProperty (song::ids::gain, -36.0, &P2().getUndoManager());
                        }
                    }
            },
            [this]
            {
                if (! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (! audioReady (H(), { "cuts" }))
                    return false;

                const double start = song::beatsToSeconds (H(), 400.0);
                const auto b = renderTracks (H(), { "cuts" }, start, start + 2.2);
                const double left = frequencyIn (b, 0.4, 0.9), right = frequencyIn (b, 1.2, 1.4);
                const double from = soundStartIn (b, 0.5, 0.1), to = soundEndIn (b);
                std::cout << "    left part " << left << " Hz, right part " << right << " Hz, from " << from << " s to " << to << " s" << std::endl;
                return std::abs (left - 330.0) < 3.0 && std::abs (right - 660.0) < 8.0
                    && std::abs (from - 1.0) < 0.005 && std::abs (to - 2.0) < 0.005;
            },
            8000, 600 });

        steps.push_back ({ "1.8 stretch: host plays 0.6 s of the right part at 2x speed, keeping the pitch -> 0.3 s long, exactly in place, still 660 Hz",
            [this]
            {
                for (auto& c : song::getClips (H()))
                    if (song::isAudioClip (c) && c[song::ids::asset].toString() == cutsHash && (double) c[song::ids::offset] > 0.5)
                    {
                        // (Not to the file's very end: there the engine's stretcher stops about
                        // a tenth of a second early - a known limit, see the README.)
                        song::setClipSpeed (H(), c, 2.0);
                        c.setProperty (song::ids::duration, 0.3, &H().getUndoManager());
                    }
            },
            [this]
            {
                if (! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (! audioReady (H(), { "cuts" }))
                    return false;

                const double start = song::beatsToSeconds (H(), 400.0);
                const auto b = renderTracks (H(), { "cuts" }, start, start + 2.2);
                const double right = frequencyIn (b, 1.1, 1.35);
                const double from = soundStartIn (b, 0.5, 0.1), to = soundEndIn (b);
                std::cout << "    right part " << right << " Hz, from " << from << " s to " << to << " s" << std::endl;
                return std::abs (right - 660.0) < 8.0 && std::abs (from - 1.0) < 0.005 && std::abs (to - 1.3) < 0.005;
            },
            8000, 600 });

        steps.push_back ({ "1.8 stretch: peer 1 switches it to 'resample' at half speed, no pitch -> like a slow record: 165 Hz, 1.2 s long",
            [this]
            {
                for (auto& c : song::getClips (P1()))
                    if (song::isAudioClip (c) && c[song::ids::asset].toString() == cutsHash && (double) c[song::ids::offset] > 0.5)
                    {
                        c.removeProperty (song::ids::pitch, &P1().getUndoManager());
                        c.setProperty (song::ids::smode, "resample", &P1().getUndoManager());
                        song::setClipSpeed (P1(), c, 0.5);
                    }
            },
            [this]
            {
                if (! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (! audioReady (H(), { "cuts" }))
                    return false;

                const double start = song::beatsToSeconds (H(), 400.0);
                const auto b = renderTracks (H(), { "cuts" }, start, start + 3.4);
                const double right = frequencyIn (b, 1.2, 2.1);
                const double from = soundStartIn (b, 0.5, 0.1), to = soundEndIn (b);
                std::cout << "    right part " << right << " Hz, from " << from << " s to " << to << " s" << std::endl;
                return std::abs (right - 165.0) < 3.0 && std::abs (from - 1.0) < 0.005 && std::abs (to - 2.2) < 0.005;
            },
            8000, 600 });

        steps.push_back ({ "1.8 time signature: host picks 3/4 -> everyone's patterns are 3 beats a bar (12 steps), the metronome counts 3",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                song::setTimeSignature (H(), { 3, 4 });
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    return song::getTimeSignature (e) == song::TimeSignature { 3, 4 }
                        && std::abs (song::getLengthBeats (pat (e, "Cut")) - 3.0) < 1.0e-9
                        && song::getNumSteps (pat (e, "Cut")) == 12
                        && e.tempoSequence.getTimeSig (0)->numerator == 3
                        && std::fmod (song::getSongLengthBeats (e), 3.0) < 1.0e-9;
                }) && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "1.8 time signature: peer 1 picks 6/8 (3 beats a bar too), then back to 4/4 -> 16 steps a bar again everywhere",
            [this]
            {
                P1().getUndoManager().beginNewTransaction();
                song::setTimeSignature (P1(), { 6, 8 });
                P1().getUndoManager().beginNewTransaction();
                song::setTimeSignature (P1(), { 4, 4 });
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    return song::getTimeSignature (e) == song::TimeSignature()
                        && song::getNumSteps (pat (e, "Cut")) == 16
                        && e.tempoSequence.getTimeSig (0)->numerator == 4
                        && ! e.state.getChildWithName (song::ids::SONGMODEL).hasProperty (song::ids::tsNum);
                }) && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "1.8 tempo changes: peer 2 adds a jump to 180 BPM at beat 144 -> everyone plays beat 160 at the same, earlier time",
            [this]
            {
                P2().getUndoManager().beginNewTransaction();
                song::addTempoChange (P2(), 144.0, 180.0);
            },
            [this]
            {
                const double base = H().tempoSequence.getTempo (0)->getBpm();
                const double expected = song::beatsToSeconds (H(), 144.0) + 16.0 * 60.0 / 180.0;
                const bool ok = everywhere ([expected] (te::Edit& e)
                {
                    const auto changes = song::getTempoChanges (e);
                    return changes.size() == 1 && std::abs (changes[0].bpm - 180.0) < 1.0e-6 && ! changes[0].ramp
                        && std::abs (song::beatsToSeconds (e, 160.0) - expected) < 1.0e-6
                        && std::abs (song::beatsToSeconds (e, 144.0) - 144.0 * 60.0 / e.tempoSequence.getTempo (0)->getBpm()) < 1.0e-6;
                });
                std::cout << "    song tempo " << base << ", beat 160 at " << song::beatsToSeconds (H(), 160.0) << " s (expected " << expected << ")" << std::endl;
                return ok && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "1.8 tempo changes: host makes it a ramp (beat 160 lands later than with the jump), moves it to beat 148, then removes it",
            [this]
            {
                H().getUndoManager().beginNewTransaction();
                song::setTempoChange (H(), 0, 144.0, 180.0, true);
            },
            [this]
            {
                // A ramp speeds up gradually from the tempo before it (from the song's start here),
                // so beat 144 comes sooner than at a steady tempo.
                const bool ramped = everywhere ([] (te::Edit& e)
                {
                    const auto changes = song::getTempoChanges (e);
                    const double steady = 144.0 * 60.0 / e.tempoSequence.getTempo (0)->getBpm();
                    return changes.size() == 1 && changes[0].ramp && song::beatsToSeconds (e, 144.0) < steady - 0.5;
                }) && hashesEqual();

                if (! ramped)
                    return false;

                song::setTempoChange (H(), 0, 148.0, 180.0, true);
                const bool moved = song::getTempoChanges (H()).size() == 1 && std::abs (song::getTempoChanges (H())[0].beat - 148.0) < 1.0e-9;
                song::removeTempoChange (H(), 0);
                std::cout << "    ramp: yes, moved: " << (moved ? "yes" : "NO") << std::endl;
                return moved && song::getTempoChanges (H()).isEmpty();
            },
            8000, 600 });

        steps.push_back ({ "1.8 tempo changes: ...gone everywhere, one steady tempo again",
            {},
            [this]
            {
                return everywhere ([] (te::Edit& e) { return song::getTempoChanges (e).isEmpty() && e.tempoSequence.getNumTempos() == 1; })
                    && hashesEqual() && compiledEqualEverywhere();
            },
            8000, 600 });

        steps.push_back ({ "1.8 bounce: host bounces the first part of the 'Cut' pattern clip -> its kick, as audio",
            [this]
            {
                juce::String sid;

                for (auto& c : song::getClips (H()))
                    if (c[song::ids::pattern].toString() == pat (H(), "Cut")[song::ids::sid].toString()
                         && std::abs ((double) c[song::ids::start] - 408.0) < 1.0e-9)
                        sid = c[song::ids::sid].toString();

                bounceDone = bounceApplied = false;
                testBouncer = std::make_unique<Bouncer> (engine, host.getAssetStore());
                const auto error = testBouncer->start (host.getSync()->createSnapshot(), sid,
                                                       [this] (const Bouncer::Result& r) { bounceResult = r; bounceDone = true; });
                std::cout << "    started: " << (error.isEmpty() ? juce::String ("yes") : error) << std::endl;
            },
            [this]
            {
                if (! bounceDone)
                    return false;

                if (! bounceApplied)
                {
                    H().getUndoManager().beginNewTransaction();
                    const auto clips = Bouncer::apply (H(), bounceResult);
                    bounceApplied = true;
                    std::cout << "    parts: " << (int) bounceResult.parts.size() << ", silent: " << bounceResult.silentChannels
                              << (bounceResult.error.isNotEmpty() ? ", error: " + bounceResult.error : juce::String())
                              << ", clips added: " << clips.size() << std::endl;

                    if (clips.size() != 1)
                        return false;
                }

                return bounceResult.parts.size() == 1 && bounceResult.parts[0].seconds >= song::beatsToSeconds (H(), 410.0) - song::beatsToSeconds (H(), 408.0) - 1.0e-3;
            },
            30000 });

        steps.push_back ({ "1.8 bounce: ...everyone has the 'Cut - Kick' audio clip in its place (same insert as the kick), the pattern clip is gone, and it sounds",
            {},
            [this]
            {
                const auto hash = bounceResult.parts.empty() ? juce::String() : bounceResult.parts[0].audio.hash;
                const bool structure = everywhere ([hash] (te::Edit& e)
                {
                    auto* kick = ch (e, "Kick");
                    auto* audio = ch (e, "Cut - Kick");
                    bool patternClipAt128 = false;

                    for (auto& c : song::getClips (e))
                        if (! song::isAudioClip (c) && std::abs ((double) c[song::ids::start] - 408.0) < 1.0e-9 && (int) c[song::ids::lane] == 15)
                            patternClipAt128 = true;

                    auto clip = audioClipOf (e, hash);
                    return kick != nullptr && audio != nullptr && clip.isValid() && ! patternClipAt128
                        && (int) clip[song::ids::lane] == 15 && std::abs ((double) clip[song::ids::start] - 408.0) < 1.0e-9
                        && song::getRoute (*audio) == song::getRoute (*kick);
                });

                if (! structure || ! hasVerified (peer1, hash) || ! hasVerified (peer2, hash) || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                // With the inserts it plays through (rendered alone, a routed channel is silent).
                juce::StringArray chain { "Cut - Kick" };

                for (auto* t = song::resolveRoute (H(), *ch (H(), "Cut - Kick")); t != nullptr && ! chain.contains (t->getName()); t = song::resolveRoute (H(), *t))
                    chain.add (t->getName());

                if (! audioReady (H(), chain))
                    return false;

                const double start = song::beatsToSeconds (H(), 408.0);
                const float peak = peakIn (renderTracks (H(), chain, start, start + 1.0), 0.0, 1.0);
                std::cout << "    bounced kick peak " << peak << std::endl;
                return peak > 0.05f;
            },
            15000, 600 });

        steps.push_back ({ "1.8 bounce: peer 1 bounces the first part of the tone (fade-in, +6 dB) -> the new file has them built in",
            [this]
            {
                juce::String sid;

                for (auto& c : song::getClips (P1()))
                    if (song::isAudioClip (c) && c[song::ids::asset].toString() == cutsHash && (double) c[song::ids::offset] < 0.5)
                    {
                        sid = c[song::ids::sid].toString();
                        c.setProperty (song::ids::gain, 6.0, &P1().getUndoManager());   // back to +6 dB
                    }

                bounceDone = bounceApplied = false;
                testBouncer = std::make_unique<Bouncer> (engine, peer1.getAssetStore());
                testBouncer->start (peer1.getSync()->createSnapshot(), sid,
                                    [this] (const Bouncer::Result& r) { bounceResult = r; bounceDone = true; });
            },
            [this]
            {
                if (! bounceDone)
                    return false;

                if (! bounceApplied)
                {
                    P1().getUndoManager().beginNewTransaction();
                    bounceApplied = true;

                    if (Bouncer::apply (P1(), bounceResult).size() != 1)
                        return false;
                }

                const auto& part = bounceResult.parts[0];
                const auto file = peer1.getAssetStore().getFile (part.audio.hash);
                const float atStart = peakOf (file, 0.0, 0.02), middle = peakOf (file, 0.5, 0.9);
                std::cout << "    bounced: " << part.seconds << " s, start " << atStart << ", middle " << middle << std::endl;
                return part.seconds >= 0.999 && atStart < 0.1f * middle && middle > 0.1f && ch (P1(), "cuts (bounce)") != nullptr;
            },
            30000 });

        steps.push_back ({ "1.8 bounce: ...everyone has it",
            {},
            [this]
            {
                const auto hash = bounceResult.parts.empty() ? juce::String() : bounceResult.parts[0].audio.hash;

                if (! everywhere ([hash] (te::Edit& e) { return audioClipOf (e, hash).isValid() && ch (e, "cuts (bounce)") != nullptr; })
                     || ! hasVerified (host, hash) || ! hasVerified (peer2, hash) || ! hashesEqual() || ! compiledEqualEverywhere()
                     || ! audioReady (H(), { "cuts (bounce)" }))
                    return false;

                // Played in the song, it sounds like the clip did: faded in, 6 dB up.
                const double start = song::beatsToSeconds (H(), 400.0);
                const auto b = renderTracks (H(), { "cuts (bounce)" }, start, start + 1.0);
                const float atStart = peakIn (b, 0.0, 0.02), middle = peakIn (b, 0.5, 0.9);
                std::cout << "    in the song: start " << atStart << ", middle " << middle << " (x" << middle / cutsBaseline << " the tone as imported)" << std::endl;
                return atStart < 0.1f * middle && middle / cutsBaseline > 1.95f && middle / cutsBaseline < 2.05f;
            },
            15000, 600 });

        steps.push_back ({ "1.8 note properties: peer 2 pans the 'Cut' kick notes hard left, tunes them +50 cents and sets their release -> the same everywhere, the kick plays on the left only",
            [this]
            {
                P2().getUndoManager().beginNewTransaction();

                // Straight to the master (an insert's stereo effects would spread it), so the
                // kick's own left / right is what's measured.
                song::setRoute (P2(), *ch (P2(), "Kick"), {});

                for (auto& n : song::getNotes (pat (P2(), "Cut"), song::getChannelID (*ch (P2(), "Kick"))))
                {
                    song::setNoteProperty (P2(), n, song::NoteProperty::pan, -100);
                    song::setNoteProperty (P2(), n, song::NoteProperty::fine, 50);
                    song::setNoteProperty (P2(), n, song::NoteProperty::release, 20);
                }
            },
            [this]
            {
                const bool everywhereSet = everywhere ([] (te::Edit& e)
                {
                    const auto notes = song::getNotes (pat (e, "Cut"), song::getChannelID (*ch (e, "Kick")));

                    for (auto& n : notes)
                        if (song::getNoteProperty (n, song::NoteProperty::pan) != -100 || song::getNoteProperty (n, song::NoteProperty::fine) != 50
                             || song::getNoteProperty (n, song::NoteProperty::release) != 20)
                            return false;

                    return ! notes.isEmpty();
                });

                if (! everywhereSet || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                const auto notes = compiled (host, "Kick").joinIntoString (" | ");

                if (! notes.contains ("@411.000+0.250*100 rv=20 pan=-100 fine=50"))
                {
                    std::cout << "    compiled: " << notes << std::endl;
                    return false;
                }

                const double start = song::beatsToSeconds (H(), 411.0);
                // The kick and the inserts it plays through (rendered alone, a routed channel is silent).
                juce::StringArray chain { "Kick" };

                for (auto* t = song::resolveRoute (H(), *ch (H(), "Kick")); t != nullptr && ! chain.contains (t->getName()); t = song::resolveRoute (H(), *t))
                    chain.add (t->getName());

                if (! audioReady (H(), chain))
                    return false;

                const float left = peakIn (renderTracks (H(), chain, start, start + 0.4, false), 0.0, 0.4);
                const float right = peakIn (renderTracks (H(), chain, start, start + 0.4, true), 0.0, 0.4);
                std::cout << "    kick at beat 411: left " << left << ", right " << right << std::endl;
                return left > 0.05f && right < 0.02f * left;
            },
            8000, 600 });

        steps.push_back ({ "1.8 backups: they carry their audio (shared, not copied twice), keep the newest, drop audio only old ones used, and open after the working copy is gone",
            [this]
            {
                fourth.newProject();
                auto assetFolder = testRoot.getChildFile ("fourth-backup-assets");
                assetFolder.deleteRecursively();
                fourth.setAssetFolder (assetFolder);
                auto& store = fourth.getAssetStore();
                auto* e = fourth.getEdit();
                const auto tone = store.importAudioFile (toneFile), voice = store.importAudioFile (voiceFile);
                song::addImportedAudio (*e, tone, "tone.wav", 0, 0.0);
                song::addImportedAudio (*e, voice, "voice.wav", 1, 0.0);

                const auto folder = backups::folderForUnsaved ("Backup test", juce::Time::getCurrentTime());
                folder.deleteRecursively();
                auto audioOf = [e] { std::vector<project::AudioFile> a;
                                     for (auto& h : song::getAssetHashes (*e)) a.push_back ({ h, song::getAssetName (*e, h), false });
                                     return a; };

                // 1: both files; 2: the voice gone from the song (the tone is already there).
                const auto first = folder.getChildFile (juce::String ("Backup test (backup 1)") + appdata::songExtension);
                const auto r1 = project::save (first, e->state, store, audioOf(), true, "test", backups::knownAudio (folder), false);
                first.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::minutes (5));
                song::removeAsset (*e, voice.hash);
                const auto second = folder.getChildFile (juce::String ("Backup test (backup 2)") + appdata::songExtension);
                const auto r2 = project::save (second, e->state, store, audioOf(), true, "test", backups::knownAudio (folder), false);
                const bool bothFiles = folder.getChildFile ("Audio/voice.flac").existsAsFile() && folder.getChildFile ("Audio/tone.flac").existsAsFile();

                // Keep only the newest: the first backup and the voice (only it used) go.
                backups::prune (folder, 1);
                const bool pruned = ! first.existsAsFile() && second.existsAsFile()
                                     && ! folder.getChildFile ("Audio/voice.flac").existsAsFile() && folder.getChildFile ("Audio/tone.flac").existsAsFile();

                // The working copy goes (as when a never-saved song closes): the backup still opens with its sound.
                store.deleteFiles ({ tone.hash });
                const bool goneHere = ! store.has (tone.hash);
                const auto loaded = project::load (second, store);
                const bool restored = loaded.ok() && store.has (tone.hash) && loaded.audioRestored == 1;
                const bool listed = backups::list (folder).size() >= 1 && backups::list (folder)[0].thisSong
                                     && backups::list (folder)[0].song == "Backup test" && appdata::isBackup (second);

                std::cout << "    saved: " << r1.ok() << r2.ok() << " (2nd wrote " << r2.audioWritten << " new files), both files: " << bothFiles
                          << ", pruned: " << pruned << ", gone from the cache: " << goneHere << ", restored from the backup: " << restored
                          << ", listed: " << listed << std::endl;
                backupCheck = r1.ok() && r1.audioWritten == 2 && r2.ok() && r2.audioWritten == 0 && bothFiles && pruned && goneHere && restored && listed;
                folder.deleteRecursively();
                fourth.newProject();
            },
            [this] { return backupCheck; } });

        //==============================================================================
        // 1.10: importing a beat from FL Studio
        steps.push_back ({ "1.10 FL beat import: an .flp with a sampler (its sound next to the .flp), an audio clip, a pattern with note properties and a trimmed clip -> the same beat here",
            {},
            [this]
            {
                // The project's folder: the .flp, and its sounds in Samples\ and Audio\ (FL saved other paths).
                auto folder = testRoot.getChildFile ("fl-beat");
                folder.deleteRecursively();
                folder.getChildFile ("Samples").createDirectory();
                folder.getChildFile ("Audio").createDirectory();
                writeTestWav (folder.getChildFile ("Samples").getChildFile ("Sad 808.wav"), 1.0, 55.0, false);
                writeTestWav (folder.getChildFile ("Audio").getChildFile ("vox.wav"), 3.0, 330.0, false);

                FlpWriter w;
                auto version = FlpWriter::text8 ("26.1.5.0");
                version.append ("\0", 1);
                w.dataEvent (199, version);
                w.dwordEvent (156, 120000);

                // A pattern's notes come first (as FL writes them): two 808 notes.
                auto note = [] (juce::MemoryOutputStream& o, int pos, int channel, int length, int key, int fine, int pan, int velocity)
                {
                    o.writeInt (pos);  o.writeShort (0x4000);  o.writeShort ((short) channel);  o.writeInt (length);
                    o.writeShort ((short) key);  o.writeShort (0);
                    o.writeByte ((char) fine);  o.writeByte (0);  o.writeByte (64);  o.writeByte (0);
                    o.writeByte ((char) pan);  o.writeByte ((char) velocity);  o.writeByte ((char) 128);  o.writeByte ((char) 128);
                };
                juce::MemoryOutputStream notes;
                note (notes, 0, 0, 48, 62, 120, 64, 100);      // beat 0, half a beat, D5
                note (notes, 96, 0, 96, 65, 240, 0, 90);       // beat 1, one beat, F5, +100 cents, hard left
                w.wordEvent (65, 1);
                w.dataEvent (224, notes.getMemoryBlock());

                // Channel 0: a sampler with FL's volume envelope on, through insert 1.
                juce::MemoryBlock levels (24, true), envelope (68, true), envelopeOn (68, true);
                const int pan = 6400, vol = 10000, on = -1;
                levels.copyFrom (&pan, 0, 4);  levels.copyFrom (&vol, 4, 4);
                envelopeOn.copyFrom (&on, 4, 4);

                w.wordEvent (64, 0);
                w.byteEvent (21, 0);
                w.textEvent (201, "");
                w.textEvent (203, "Sad 808");
                w.textEvent (196, "%FLStudioFactoryData%\\Projects\\my beat\\Samples\\Sad 808.wav");
                w.wordEvent (104, 1);
                w.dataEvent (219, levels);
                w.dataEvent (218, envelope);
                w.dataEvent (218, envelopeOn);
                w.wordEvent (65, 1);
                w.textEvent (193, "808 line");

                // Channel 1: an audio clip channel to the master.
                w.wordEvent (64, 1);
                w.byteEvent (21, 4);
                w.textEvent (201, "");
                w.textEvent (203, "Vox");
                w.textEvent (196, "%FLStudioFactoryData%\\Projects\\my beat\\Audio\\vox.wav");
                w.wordEvent (104, 0);
                w.dataEvent (219, levels);

                // The playlist: the pattern on track 3 at beat 4, trimmed to start 1 beat in and
                // last 6 beats; the audio on track 4 at beat 2, starting 0.5 s into the file, 2 beats long.
                auto item = [] (juce::MemoryOutputStream& o, int pos, int what, int length, int track, const void* trim)
                {
                    o.writeInt (pos);  o.writeShort (20480);  o.writeShort ((short) what);  o.writeInt (length);
                    o.writeShort ((short) (499 - track));  o.writeShort (0);
                    for (int i = 0; i < 8; ++i) o.writeByte (0);
                    o.write (trim, 8);
                    for (int i = 32; i < 88; ++i) o.writeByte (0);
                };
                juce::MemoryOutputStream items;
                const int patternTrim[] = { 96, 672 };
                const float audioTrim[] = { 500.0f, 1500.0f };
                item (items, 384, 20480 + 1, 576, 2, patternTrim);
                item (items, 192, 1, 192, 3, audioTrim);
                w.wordEvent (99, 0);
                w.dataEvent (233, items.getMemoryBlock());

                // A mixer: the master and insert 1 (where the 808 plays).
                auto header = [&w] (const juce::String& name) { if (name.isNotEmpty()) w.textEvent (204, name); w.dataEvent (236, juce::MemoryBlock (12, true)); };
                auto routing = [&w] (std::initializer_list<int> to) { juce::MemoryBlock r (4, true); for (auto t : to) r[(size_t) t] = 1; w.dataEvent (235, r); };
                header ({});   routing ({});
                header ({});   routing ({ 0 });

                const auto flp = folder.getChildFile ("my beat.flp");
                const auto bytes = w.finish();
                flp.replaceWithData (bytes.getData(), bytes.getSize());

                flimport::Project project;
                const auto error = flimport::parse (flp, project);
                project.findSamples (flp);

                auto edit = te::createEmptyEdit (engine, juce::File());
                for (auto* t : te::getAudioTracks (*edit))
                    edit->deleteTrack (t);

                flimport::Options options;
                options.inserts = project.getUsedInserts();
                options.assets = &fourth.getAssetStore();
                const auto report = flimport::apply (*edit, project, options);

                auto* bass = ch (*edit, "Sad 808");
                auto* vox = ch (*edit, "Vox");
                auto* sampler = bass != nullptr ? dynamic_cast<SamplerPlugin*> (song::getInstrument (*bass)) : nullptr;
                auto pattern = pat (*edit, "808 line");
                auto* insert1 = song::findInsertNamed (*edit, "Insert 1");
                const auto bassNotes = bass != nullptr && pattern.isValid() ? song::getNotes (pattern, song::getChannelID (*bass)) : juce::Array<juce::ValueTree>();

                juce::ValueTree patternClip, audioClip;
                for (auto& c : song::getClips (*edit))
                    (song::isAudioClip (c) ? audioClip : patternClip) = c;

                auto near = [] (double a, double b) { return std::abs (a - b) < 1.0e-6; };
                const bool channels = bass != nullptr && vox != nullptr && sampler != nullptr && sampler->getAsset().isNotEmpty()
                                      && sampler->noteLengthValue.get() && insert1 != nullptr && song::getRoute (*bass) == insert1->itemID.toString();
                const bool notesOk = bassNotes.size() == 2 && near (bassNotes[0][song::ids::b], 0.0) && near (bassNotes[0][song::ids::l], 0.5)
                                     && (int) bassNotes[0][song::ids::p] == 62 && near (bassNotes[1][song::ids::b], 1.0)
                                     && (int) bassNotes[1][song::ids::p] == 65 && (int) bassNotes[1][song::ids::v] == 90
                                     && song::getNoteProperty (bassNotes[1], song::NoteProperty::pan) == -100
                                     && song::getNoteProperty (bassNotes[1], song::NoteProperty::fine) == 100;
                const bool clipsOk = patternClip.isValid() && (int) patternClip[song::ids::lane] == 2 && near (patternClip[song::ids::start], 4.0)
                                     && near (song::getClipLength (*edit, patternClip), 6.0) && near (song::getPatternOffset (patternClip), 1.0)
                                     && audioClip.isValid() && (int) audioClip[song::ids::lane] == 3 && near (audioClip[song::ids::start], 2.0)
                                     && near (audioClip[song::ids::offset], 0.5) && near (audioClip[song::ids::duration], 1.0)   // 2 beats at 120 BPM
                                     && audioClip[song::ids::ch].toString() == song::getChannelID (*vox);

                std::cout << "    parse: " << (error.isEmpty() ? "ok" : error) << ", sounds found " << project.countSamples (true) << "/" << project.countSamples (false)
                          << ", channels " << (channels ? "right" : "WRONG") << ", notes " << (notesOk ? "right" : "WRONG")
                          << ", clips " << (clipsOk ? "right" : "WRONG") << "\n    " << report.toText().upToFirstOccurrenceOf ("\n\n", false, false).replace ("\n", " | ") << std::endl;
                return error.isEmpty() && project.countSamples (true) == 2 && channels && notesOk && clipsOk;
            },
            10000 });

        //==============================================================================
        // 1.11: VST2 plugins, and FL's own effects with their settings
        steps.push_back ({ "1.11 VST2: the test plugin's VST2 loads from an FL beat (found by FL's path, with FL's saved chunk), sounds right and keeps its state",
            [this] { checks["vst2"] = [this]
            {
                auto& pm = engine.getPluginManager();
                juce::AudioPluginFormat* vst2 = nullptr;

                for (auto* f : pm.pluginFormatManager.getFormats())
                    if (f->getName() == "VST")
                        vst2 = f;

                juce::OwnedArray<juce::PluginDescription> found;
                std::cout << "    .. VST2 file: " << vst2File.getFullPathName() << std::endl;

                if (vst2 != nullptr)
                    vst2->findAllTypesForFile (found, vst2File.getFullPathName());

                if (found.isEmpty())
                {
                    std::cout << "    no VST2 host or plugin: " << (vst2 == nullptr ? "VST format missing" : vst2File.getFullPathName()) << std::endl;
                    return false;
                }

                const auto desc = *found[0];
                std::cout << "    .. scanned: " << desc.name << " (" << desc.pluginFormatName << ", id " << desc.uniqueId << ")" << std::endl;

                // The plugin's own chunk with the knob at 0.35 (what FL saves, inside its header).
                juce::MemoryBlock chunk;
                juce::String error;

                if (auto instance = vst2->createInstanceFromDescription (desc, 44100.0, 512, error))
                {
                    std::cout << "    .. instance made" << std::endl;
                    instance->getParameters()[0]->setValue (0.35f);
                    juce::MemoryBlock fxb;
                    instance->getStateInformation (fxb);
                    std::cout << "    .. state: " << (int) fxb.getSize() << " bytes" << std::endl;

                    if (fxb.getSize() > 160)
                        chunk = juce::MemoryBlock (addBytesToPointer (fxb.getData(), 160), fxb.getSize() - 160);
                }

                juce::MemoryOutputStream flState;
                flState.writeInt (-9);  flState.writeInt (0);  flState.writeByte (0);
                flState.writeInt ((int) chunk.getSize());
                flState.writeRepeatedByte (0, 8);
                flState.write (chunk.getData(), chunk.getSize());

                juce::MemoryOutputStream wrapper;
                wrapper.writeInt (12);
                juce::MemoryBlock kind (4, true), id (4, true);
                const auto uid = (juce::uint32) desc.uniqueId;
                id.copyFrom (&uid, 0, 4);
                FlpWriter::record (wrapper, 50, kind);                 // 0 = VST2
                FlpWriter::record (wrapper, 51, id);
                FlpWriter::record (wrapper, 54, FlpWriter::text8 ("SyncTestGain"));
                FlpWriter::record (wrapper, 55, FlpWriter::text8 (vst2File.getFullPathName()));
                FlpWriter::record (wrapper, 53, flState.getMemoryBlock());

                FlpWriter w;
                w.dwordEvent (156, 120000);
                auto header = [&w] (const juce::String& name) { w.textEvent (204, name); w.dataEvent (236, juce::MemoryBlock (12, true)); };
                auto routing = [&w] (bool toMaster) { juce::MemoryBlock r (8, true); r[0] = toMaster ? 1 : 0; w.dataEvent (235, r); };
                header ("Master");  for (int i = 0; i < 10; ++i) w.wordEvent (98, i);  routing (false);
                header ("Keys");
                w.textEvent (201, "Fruity Wrapper");
                w.dataEvent (213, wrapper.getMemoryBlock());
                w.wordEvent (98, 0);
                for (int i = 1; i < 10; ++i) w.wordEvent (98, i);
                routing (true);

                // Not scanned yet: the import finds it where FL had it.
                for (auto& d : pm.knownPluginList.getTypes())
                    if (d.pluginFormatName == "VST")
                        pm.knownPluginList.removeType (d);

                flimport::Project project;
                const auto parseError = flimport::parse (w.finish(), project);
                auto edit = te::createEmptyEdit (engine, juce::File());
                for (auto* t : te::getAudioTracks (*edit))
                    edit->deleteTrack (t);

                flimport::Options options;
                options.inserts = project.getUsedInserts();
                std::cout << "    .. importing" << std::endl;
                const auto report = flimport::apply (*edit, project, options);
                std::cout << "    .. imported" << std::endl;

                auto* keys = insert (*edit, "Keys");
                auto effects = keys != nullptr ? song::getInsertEffects (*keys) : juce::Array<te::Plugin*>();
                auto* ext = effects.size() == 1 ? dynamic_cast<te::ExternalPlugin*> (effects[0]) : nullptr;
                auto* pi = ext != nullptr ? ext->getAudioPluginInstance() : nullptr;
                const float knob = pi != nullptr ? pi->getParameters()[0]->getValue() : -1.0f;
                const bool isVST2 = ext != nullptr && ext->desc.pluginFormatName == "VST";

                // Its sound: the gain doubles the knob (0.35 -> x0.7).
                float out = -1.0f;

                if (pi != nullptr)
                {
                    juce::AudioBuffer<float> buffer (2, 256);
                    juce::MidiBuffer midi;
                    buffer.clear();
                    for (int c = 0; c < 2; ++c)
                        juce::FloatVectorOperations::fill (buffer.getWritePointer (c), 0.5f, 256);

                    std::cout << "    .. playing" << std::endl;
                    pi->prepareToPlay (44100.0, 256);
                    pi->processBlock (buffer, midi);
                    out = buffer.getSample (0, 100);
                }

                // Its state as the song saves it reloads into a new instance with the same knob.
                float reloaded = -1.0f;

                if (ext != nullptr)
                {
                    std::cout << "    .. saving" << std::endl;
                    pi->getParameters()[0]->setValue (0.6f);
                    ext->flushPluginStateToValueTree();
                    juce::MemoryBlock saved;
                    saved.fromBase64Encoding (ext->state[te::IDs::state].toString());

                    if (auto again = vst2->createInstanceFromDescription (desc, 44100.0, 512, error))
                    {
                        again->setStateInformation (saved.getData(), (int) saved.getSize());
                        reloaded = again->getParameters()[0]->getValue();
                    }
                }

                const bool inList = [&pm, &desc]
                {
                    for (auto& d : pm.knownPluginList.getTypes())
                        if (d.pluginFormatName == "VST" && d.uniqueId == desc.uniqueId)
                            return true;
                    return false;
                }();

                std::cout << "    parse: " << (parseError.isEmpty() ? "ok" : parseError) << ", chunk " << (int) chunk.getSize()
                          << " bytes, scanned from FL's path: " << (inList ? "yes" : "NO") << ", loaded as VST2: " << (isVST2 ? "yes" : "NO")
                          << ", knob " << knob << " (want 0.35), 0.5 in -> " << out << " out (want 0.35)"
                          << ", saved and reloaded knob " << reloaded << " (want 0.6), with settings " << report.effectsWithSettings << std::endl;

                return parseError.isEmpty() && chunk.getSize() > 0 && inList && isVST2
                    && std::abs (knob - 0.35f) < 1.0e-3f && std::abs (out - 0.35f) < 1.0e-3f
                    && std::abs (reloaded - 0.6f) < 1.0e-3f && report.effectsWithSettings == 1;
            }(); },
            [this] { return checks["vst2"]; },
            20000, 0, ! vst2File.exists() });

        steps.push_back ({ "1.11 FL effects: Fruity Delay 3, Reeverb 2 and Parametric EQ 2 come in as ours with the settings FL saved (a real hi-hat delay)",
            [this] { checks["flfx"] = [this]
            {
                auto ints = [] (std::initializer_list<int> values)
                {
                    juce::MemoryOutputStream o;
                    for (auto v : values)
                        o.writeInt (v);
                    return o.getMemoryBlock();
                };

                // From a real project (FL 26.1.5): a 1/4 tempo delay on hats, 42% feedback, 32% wet.
                auto delay = ints ({ 1, 6000, 1, 1, 0, 192, 3000, 500, 6000, 3000, 0, 0, 0, 0, 1, 2550, 0, 6000, 0, 0, 0, 0, 60000, 6000, 1894, 0, 6000 });
                auto reverb = ints ({ 10001, 75, 40, 192, 50, 100, 15, 40, 100, 500, 0, 117, 64, 106, 684, 0 });
                const char reverbSwitches[] = { 1, 0 };
                reverb.append (reverbSwitches, 2);

                std::vector<int> eq (72, 0);
                eq[0] = 8;
                eq[1] = -1800;                                                    // band 1: -18 dB
                const int freqs[] = { 0, 18204, 25486, 32768, 40050, 47332, 54613 };
                const int types[] = { 5, 6, 6, 6, 6, 6, 7 };
                for (int b = 0; b < 7; ++b)
                {
                    eq[(size_t) (8 + b)] = freqs[b];
                    eq[(size_t) (15 + b)] = b == 0 || b == 6 ? 40124 : 25412;
                    eq[(size_t) (22 + b)] = types[b];
                }
                eq[(size_t) (8 + 3)] = 32768;
                eq[(size_t) (1 + 3)] = 450;                                       // band 4: +4.5 dB at 632 Hz
                juce::MemoryOutputStream eqData;
                for (auto v : eq) eqData.writeInt (v);

                FlpWriter w;
                w.dwordEvent (156, 158000);
                auto header = [&w] (const juce::String& name) { w.textEvent (204, name); w.dataEvent (236, juce::MemoryBlock (12, true)); };
                auto routing = [&w] (bool toMaster) { juce::MemoryBlock r (8, true); r[0] = toMaster ? 1 : 0; w.dataEvent (235, r); };
                header ("Master");  for (int i = 0; i < 10; ++i) w.wordEvent (98, i);  routing (false);
                header ("Hats");
                w.textEvent (201, "Fruity Delay 3");          w.dataEvent (213, delay);                    w.wordEvent (98, 0);
                w.textEvent (201, "Fruity Reeverb 2");        w.dataEvent (213, reverb);                   w.wordEvent (98, 1);
                w.textEvent (201, "Fruity Parametric EQ 2");  w.dataEvent (213, eqData.getMemoryBlock());  w.wordEvent (98, 2);
                for (int i = 3; i < 10; ++i) w.wordEvent (98, i);
                routing (true);

                flimport::Project project;
                const auto parseError = flimport::parse (w.finish(), project);
                auto edit = te::createEmptyEdit (engine, juce::File());
                for (auto* t : te::getAudioTracks (*edit))
                    edit->deleteTrack (t);

                flimport::Options options;
                options.inserts = project.getUsedInserts();
                const auto report = flimport::apply (*edit, project, options);

                auto* hats = insert (*edit, "Hats");
                auto fx = hats != nullptr ? song::getInsertEffects (*hats) : juce::Array<te::Plugin*>();
                auto value = [&fx] (int index, const char* id) -> float
                {
                    if (auto* p = fx[index])
                        if (auto param = p->getAutomatableParameterByID (id))
                            return param->getCurrentValue();
                    return -1000.0f;
                };
                auto near = [] (float a, float b, float within) { return std::abs (a - b) <= within; };

                const bool kinds = fx.size() == 3 && dynamic_cast<DelayPlugin*> (fx[0]) != nullptr
                                && dynamic_cast<ReverbPlugin*> (fx[1]) != nullptr && dynamic_cast<ParametricEQPlugin*> (fx[2]) != nullptr;
                const bool delayOk = kinds && value (0, "sync") == 1.0f && juce::String (dly::divisions[(int) value (0, "division")].name) == "1/4"
                                  && value (0, "model") == (float) dly::stereo && value (0, "distType") == (float) dly::saturate
                                  && near (value (0, "feedback"), 0.425f * 1.2f, 0.01f) && near (value (0, "wet"), 0.316f, 0.01f)
                                  && near (value (0, "dry"), 1.0f, 0.001f) && near (value (0, "cutoff"), 20000.0f, 1.0f)
                                  && value (0, "filterType") == (float) dly::lowPass && near (value (0, "distLevel"), 0.0f, 0.001f);
                const bool reverbOk = kinds && value (1, "tempo") == 1.0f && near (value (1, "decay"), 1.5f, 0.01f)
                                   && near (value (1, "wet"), 106.0f / 128.0f, 0.01f) && near (value (1, "dry"), 117.0f / 128.0f, 0.01f)
                                   && near (value (1, "predelay"), 380.0f, 1.0f) && near (value (1, "lowCut"), 75.0f, 0.5f);
                const bool eqOk = kinds && value (2, "type1") == (float) peq::lowShelf && near (value (2, "gain1"), -18.0f, 0.01f)
                               && near (value (2, "freq1"), 20.0f, 0.5f) && near (value (2, "gain4"), 4.5f, 0.01f)
                               && near (value (2, "freq4"), 632.0f, 2.0f) && value (2, "type7") == (float) peq::highShelf;

                std::cout << "    parse: " << (parseError.isEmpty() ? "ok" : parseError) << ", ours in place: " << (kinds ? "yes" : "NO")
                          << ", delay " << (delayOk ? "right" : "WRONG") << " (feedback " << value (0, "feedback") << ", wet " << value (0, "wet") << ")"
                          << ", reverb " << (reverbOk ? "right" : "WRONG") << " (predelay " << value (1, "predelay") << " ms)"
                          << ", EQ " << (eqOk ? "right" : "WRONG") << ", with settings " << report.effectsWithSettings << std::endl;

                return parseError.isEmpty() && delayOk && reverbOk && eqOk && report.effectsWithSettings == 3 && report.defaultSettings.isEmpty();
            }(); },
            [this] { return checks["flfx"]; },
            10000 });

        //==============================================================================
        // 1.12: four more effects of our own (measured), and FL's versions of them imported
        steps.push_back ({ "1.12 effects measured: Compressor turns a loud tone down by its ratio, Filter muffles highs, Enhancer brings a quiet sound up, Flanger sweeps",
            {},
            [this]
            {
                const double sr = 44100.0;
                bool ok = true;
                auto check = [&ok] (const juce::String& what, double got, double lo, double hi)
                {
                    const bool good = got >= lo && got <= hi && ! std::isnan (got);
                    std::cout << "    " << what << ": " << juce::String (got, 4) << (good ? "" : "   <-- expected " + juce::String (lo) + " .. " + juce::String (hi)) << std::endl;
                    ok = ok && good;
                };

                auto edit = te::createEmptyEdit (engine, juce::File());

                // A sine through a plugin for a second; returns the peak of the last 0.2 s.
                auto peakThrough = [&] (te::Plugin& plugin, double hz, float level, std::vector<float>* all = nullptr)
                {
                    plugin.initialise ({ te::TimePosition(), sr, 512 });
                    plugin.reset();
                    juce::AudioBuffer<float> buffer (2, 512);
                    te::MidiMessageArray midi;
                    float peak = 0.0f;
                    const int total = (int) sr;

                    for (int pos = 0; pos < total; pos += 512)
                    {
                        for (int i = 0; i < 512; ++i)
                            for (int c = 0; c < 2; ++c)
                                buffer.setSample (c, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (pos + i) / sr));

                        te::PluginRenderContext rc (&buffer, 0, 512, &midi, 0.0, {}, true, false, false, false);
                        plugin.applyToBuffer (rc);

                        for (int i = 0; i < 512; ++i)
                        {
                            if (pos + i > total - (int) (0.2 * sr))
                                peak = juce::jmax (peak, std::abs (buffer.getSample (0, i)));

                            if (all != nullptr)
                                all->push_back (buffer.getSample (0, i));
                        }
                    }

                    return peak;
                };
                auto set = [] (te::Plugin& p, const char* id, float v) { p.getAutomatableParameterByID (id)->setParameter (v, juce::sendNotificationSync); };
                auto db = [] (float g) { return juce::Decibels::gainToDecibels (g, -100.0f); };

                auto comp = edit->getPluginCache().createNewPlugin (CompressorFxPlugin::xmlTypeName, {});
                set (*comp, "threshold", -20.0f);  set (*comp, "ratio", 4.0f);  set (*comp, "compType", 0.0f);
                set (*comp, "attack", 1.0f);  set (*comp, "release", 50.0f);  set (*comp, "gain", 0.0f);
                // A 0 dB peak sine is 20 dB over: 4:1 leaves 5 dB over, so about -15 dB.
                check ("compressor 4:1 at -20 dB, 0 dB in -> dB out", db (peakThrough (*comp, 200.0, 1.0f)), -16.5, -13.5);
                check ("compressor, -30 dB in (under the threshold) -> dB out", db (peakThrough (*comp, 200.0, juce::Decibels::decibelsToGain (-30.0f))), -30.5, -29.5);

                auto filter = edit->getPluginCache().createNewPlugin (FilterFxPlugin::xmlTypeName, {});
                set (*filter, "cutoff", 500.0f);  set (*filter, "resonance", 0.0f);  set (*filter, "stages", 2.0f);
                check ("filter low pass 500 Hz x2: 100 Hz -> dB", db (peakThrough (*filter, 100.0, 0.5f) / 0.5f), -2.0, 1.0);
                check ("filter low pass 500 Hz x2: 5 kHz -> dB", db (peakThrough (*filter, 5000.0, 0.5f) / 0.5f), -100.0, -30.0);
                set (*filter, "low", 0.0f);  set (*filter, "high", 1.0f);
                check ("filter high pass 500 Hz x2: 100 Hz -> dB", db (peakThrough (*filter, 100.0, 0.5f) / 0.5f), -100.0, -20.0);

                auto enhancer = edit->getPluginCache().createNewPlugin (EnhancerPlugin::xmlTypeName, {});
                set (*enhancer, "amount", 0.0f);
                const float quietPlain = peakThrough (*enhancer, 1000.0, 0.03f);
                set (*enhancer, "amount", 1.0f);  set (*enhancer, "mode", 2.0f);
                const float quietUp = peakThrough (*enhancer, 1000.0, 0.03f);
                check ("enhancer amount 0: a -30 dB tone comes out at (dB)", db (quietPlain) - db (0.03f), -0.5, 0.5);
                check ("enhancer C 100%: the same tone comes up by (dB)", db (quietUp) - db (0.03f), 3.0, 14.0);
                check ("enhancer C 100%: a 0 dB tone stays under the ceiling (peak)", peakThrough (*enhancer, 1000.0, 1.0f), 0.5, 0.99);

                auto flanger = edit->getPluginCache().createNewPlugin (FlangerPlugin::xmlTypeName, {});
                std::vector<float> swept;
                peakThrough (*flanger, 3000.0, 0.5f, &swept);
                // A comb filter sweeping over the tone: its loudness goes up and down over time.
                float lo = 10.0f, hi = 0.0f;

                for (size_t start = (size_t) (0.2 * sr); start + 441 < swept.size(); start += 441)
                {
                    float p = 0.0f;
                    for (size_t i = start; i < start + 441; ++i) p = juce::jmax (p, std::abs (swept[i]));
                    lo = juce::jmin (lo, p);  hi = juce::jmax (hi, p);
                }

                check ("flanger: a steady tone swings by (dB)", db (hi) - db (juce::jmax (1.0e-6f, lo)), 3.0, 100.0);
                return ok;
            },
            15000 });

        steps.push_back ({ "1.12 FL effects: a vocal chain's Limiter (gate), Compressor, Soundgoodizer, Flangus (33% mix) and Love Philter come in as ours with their settings",
            [this] { checks["flfx2"] = [this]
            {
                auto ints = [] (std::initializer_list<int> values)
                {
                    juce::MemoryOutputStream o;
                    for (auto v : values)
                        o.writeInt (v);
                    return o.getMemoryBlock();
                };

                // From the user's template (FL 26.1.5): the Adlib insert's chain.
                auto limiter = ints ({ 7, 1000, 1000, 1000, 1115, 3, 5000, 3, 2882, 1000, 568, 386, 0, 6666, 1, 650, 0, 39, 6523,
                                       1000, 1000, 1000, 1115, 3, 5000, 3, 2882, 1000, 0, 0, 0, 6666, 1, 650, 1000, 0, 4187 });
                auto compressor = ints ({ 3, -181, 80, 71, 99, 493, 3, 1, 0 });
                auto goodizer = ints ({ 3, 2, 467 });
                auto flangus = ints ({ 4, 512, 768, 8, 128, -32, 96, 96 });

                // Love Philter: u32 version, then zlib: 62 bytes, a preset name, the first unit.
                juce::MemoryOutputStream philter;
                philter.writeInt (786435);
                {
                    juce::MemoryOutputStream raw;
                    raw.writeRepeatedByte (0, 62);
                    const juce::String name ("Preset name");
                    raw.writeByte ((char) name.length());
                    for (auto p = name.getCharPointer(); ! p.isEmpty();)
                        raw.writeShort ((short) p.getAndAdvance());
                    for (int v : { 128, 128, 128, 128, 0, 0, 128, 128, 0, 128, 128, 0, 1, 0 })
                        raw.writeInt (v);
                    raw.writeRepeatedByte (0, 64);

                    juce::GZIPCompressorOutputStream zlib (philter, 9, juce::GZIPCompressorOutputStream::windowBitsRaw + 15);
                    zlib.write (raw.getData(), raw.getDataSize());
                }

                FlpWriter w;
                w.dwordEvent (156, 156000);
                auto header = [&w] (const juce::String& name) { w.textEvent (204, name); w.dataEvent (236, juce::MemoryBlock (12, true)); };
                auto routing = [&w] (bool toMaster) { juce::MemoryBlock r (8, true); r[0] = toMaster ? 1 : 0; w.dataEvent (235, r); };
                header ("Master");  for (int i = 0; i < 10; ++i) w.wordEvent (98, i);  routing (false);
                header ("Adlib");
                int slot = 0;

                for (auto [kind, data] : { std::pair<const char*, juce::MemoryBlock> { "Fruity Limiter", limiter }, { "Fruity Compressor", compressor },
                                           { "Soundgoodizer", goodizer }, { "Fruity Love Philter", philter.getMemoryBlock() }, { "Fruity Flangus", flangus } })
                {
                    w.textEvent (201, kind);
                    w.dataEvent (213, data);
                    w.wordEvent (98, slot++);
                }

                for (int i = slot; i < 10; ++i) w.wordEvent (98, i);
                routing (true);

                // The Flangus at 33% (FL's slot mix, in the mixer's parameter table: slot 4 mix).
                juce::MemoryOutputStream params;
                params.writeInt (0);  params.writeByte (1);  params.writeByte (0x1f);
                params.writeShort ((short) (0x7000 + 1 * 64 + 4));  params.writeInt (4224);
                w.dataEvent (225, params.getMemoryBlock());

                flimport::Project project;
                const auto parseError = flimport::parse (w.finish(), project);
                auto edit = te::createEmptyEdit (engine, juce::File());
                for (auto* t : te::getAudioTracks (*edit))
                    edit->deleteTrack (t);

                flimport::Options options;
                options.inserts = project.getUsedInserts();
                const auto report = flimport::apply (*edit, project, options);

                auto* adlib = insert (*edit, "Adlib");
                auto fx = adlib != nullptr ? song::getInsertEffects (*adlib) : juce::Array<te::Plugin*>();
                auto value = [&fx] (int index, const char* id) -> float
                {
                    if (auto* p = fx[index])
                        if (auto param = p->getAutomatableParameterByID (id))
                            return param->getCurrentValue();
                    return -1000.0f;
                };
                auto near = [] (float a, float b, float within) { return std::abs (a - b) <= within; };
                const double flangusMix = project.inserts[1].effects.size() == 5 ? project.inserts[1].effects[4].mix : -1.0;

                const bool kinds = fx.size() == 5 && dynamic_cast<LimiterPlugin*> (fx[0]) != nullptr && dynamic_cast<CompressorFxPlugin*> (fx[1]) != nullptr
                                && dynamic_cast<EnhancerPlugin*> (fx[2]) != nullptr && dynamic_cast<FilterFxPlugin*> (fx[3]) != nullptr
                                && dynamic_cast<FlangerPlugin*> (fx[4]) != nullptr;
                const bool limiterOk = kinds && near (value (0, "gateGain"), 0.0f, 0.001f) && near (value (0, "gateThresh"), -28.2f, 0.2f)
                                    && near (value (0, "limGain"), 0.0f, 0.01f) && near (value (0, "ceiling"), 0.0f, 0.01f);
                const bool compOk = kinds && near (value (1, "threshold"), -18.1f, 0.01f) && near (value (1, "ratio"), 8.0f, 0.01f)
                                 && near (value (1, "gain"), 7.1f, 0.01f) && near (value (1, "attack"), 9.9f, 0.05f)
                                 && near (value (1, "release"), 493.0f, 0.5f) && value (1, "compType") == 3.0f;
                const bool goodOk = kinds && value (2, "mode") == 2.0f && near (value (2, "amount"), 0.467f, 0.001f);
                const bool filterOk = kinds && value (3, "stages") == 1.0f && near (value (3, "low"), 1.0f, 0.001f) && near (value (3, "band"), 0.0f, 0.001f)
                                   && near (fx[3]->getAutomatableParameterByID ("cutoff")->getCurrentNormalisedValue(), 0.5f, 0.01f)
                                   && near (value (3, "inVol"), 0.0f, 0.01f) && near (value (3, "outVol"), 0.0f, 0.01f);
                const float m = (float) flangusMix;
                const bool flangerOk = kinds && value (4, "order") == 4.0f && near (value (4, "depth"), 0.5f, 0.001f)
                                    && near (value (4, "wet"), 0.75f * m, 0.01f) && near (value (4, "dry"), (1.0f - m) + 0.75f * m, 0.01f);

                std::cout << "    parse: " << (parseError.isEmpty() ? "ok" : parseError) << ", ours: " << (kinds ? "yes" : "NO")
                          << ", limiter " << (limiterOk ? "right" : "WRONG") << " (gate " << value (0, "gateThresh") << " dB)"
                          << ", compressor " << (compOk ? "right" : "WRONG") << ", enhancer " << (goodOk ? "right" : "WRONG")
                          << ", filter " << (filterOk ? "right" : "WRONG") << ", flanger " << (flangerOk ? "right" : "WRONG")
                          << " (slot mix " << flangusMix << ", wet " << value (4, "wet") << "), with settings " << report.effectsWithSettings << std::endl;

                return parseError.isEmpty() && limiterOk && compOk && goodOk && filterOk && flangerOk && report.effectsWithSettings == 5
                    && report.notAvailable.isEmpty();
            }(); },
            [this] { return checks["flfx2"]; },
            10000 });

        //==============================================================================
        // 1.12: a playlist track's arm and monitor buttons (this PC's recording)
        steps.push_back ({ "1.12 track buttons: peer 2's track 'Vocals' records through its insert; its monitor switch turns the input's monitoring off and on, and is remembered",
            {},
            [this]
            {
                auto& rec = peer2.getRecorder();
                auto inputs = rec.getInputs();

                if (inputs.isEmpty() || insert (P2(), "Vocals") == nullptr)
                {
                    std::cout << "    no input or no Vocals insert" << std::endl;
                    return false;
                }

                if (rec.getInputFor (vocalsID).isEmpty())
                    rec.setInputFor (vocalsID, inputs[0].id);

                // The track named like the insert records through it (what the header's arm uses).
                song::setLaneName (P2(), 40, "Vocals");
                const auto target = song::getLaneTarget (P2(), 40);
                song::setLaneName (P2(), 40, {});

                auto monitorMode = [this, &rec]
                {
                    for (auto* d : engine.getDeviceManager().getWaveInputDevices())
                        if (d->getDeviceID() == rec.getInputFor (vocalsID))
                            return d->getMonitorMode();

                    return te::InputDevice::MonitorMode::automatic;
                };

                const bool wasOn = monitorMode() == te::InputDevice::MonitorMode::on;
                rec.setMonitored (vocalsID, false);
                const bool off = monitorMode() == te::InputDevice::MonitorMode::off && ! rec.isMonitored (vocalsID);
                const auto saved = rec.saveChoices();
                rec.setMonitored (vocalsID, true);
                const bool backOn = monitorMode() == te::InputDevice::MonitorMode::on && rec.isMonitored (vocalsID);

                std::cout << "    track -> insert: " << (target.routes && target.insertID == vocalsID ? "Vocals" : "WRONG")
                          << ", monitoring on / off / on: " << (wasOn ? "on" : "OFF") << " / " << (off ? "off" : "ON") << " / " << (backOn ? "on" : "OFF")
                          << ", remembered: " << (saved.contains ("\tm0") ? "yes" : "NO") << std::endl;

                return target.routes && target.insertID == vocalsID && wasOn && off && backOn && saved.contains ("\tm0");
            },
            8000 });

        //==============================================================================
        // 1.12: the master volume's automation clip (its knob is "master volume" in the engine)
        steps.push_back ({ "1.12 master automation: 'Create automation clip' on the master fader makes a clip for the master volume -> on every PC",
            [this]
            {
                host.setSongMode (true);
                auto& master = *H().getMasterVolumePlugin();
                const auto paramID = EditorState::automationParamID (master, "volume");
                auto clip = song::addAutomationClip (H(), master.itemID.toString(), paramID, song::findFreeLane (H(), 1000.0, 4.0), 1000.0, 4.0, 1.0f);
                masterAutoSid = clip[song::ids::sid].toString();
                song::moveAutomationPoint (H(), song::getAutomationPoints (clip).getLast(), 4.0, 0.0f);
            },
            [this]
            {
                return hashesEqual() && everywhere ([this] (te::Edit& e)
                {
                    for (auto c : song::getClips (e))
                        if (c[song::ids::sid].toString() == masterAutoSid)
                            return song::findAutomationTarget (e, c) == e.getMasterVolumePlugin()->volParam.get();

                    return false;
                });
            },
            8000, 600 });

        steps.push_back ({ "1.12 master automation: ...the master volume follows it in SONG mode (then the clip goes)",
            {},
            [this]
            {
                auto& vol = *H().getMasterVolumePlugin()->volParam;
                const auto curve = host.getCompiler()->describeCompiledAutomation (vol);

                if (curve.size() < 2)
                    return false;

                const float original = vol.getCurrentValue();
                vol.updateToFollowCurve (te::TimePosition::fromSeconds (timeOfBeat (H(), 1000.5)));
                const float early = vol.getCurrentNormalisedValue();
                vol.updateToFollowCurve (te::TimePosition::fromSeconds (timeOfBeat (H(), 1003.5)));
                const float late = vol.getCurrentNormalisedValue();
                std::cout << "    master volume at beat 1000.5: " << early << ", at 1003.5: " << late << std::endl;

                for (auto c : song::getClips (H()))
                    if (c[song::ids::sid].toString() == masterAutoSid)
                    {
                        song::removeClip (H(), c);
                        break;
                    }

                vol.setParameter (original, juce::dontSendNotification);
                return std::abs (early - 0.875f) < 0.02f && std::abs (late - 0.125f) < 0.02f;
            },
            8000, 600 });

        //==============================================================================
        // 1.9: the playlist opens a pattern on the instrument last edited in it
        steps.push_back ({ "1.9 pattern click: a pattern opens on the instrument you last edited in it (else the first one with notes)",
            {},
            [this]
            {
                EditorState st (host);
                auto cut = pat (H(), "Cut");
                const auto kickID = song::getChannelID (*ch (H(), "Kick"));

                // Never edited here: the first instrument with notes in it (the kick).
                st.selectedChannel = {};
                st.viewPattern (cut[song::ids::sid].toString());
                const bool first = st.selectedChannel == kickID;

                // Edit another instrument in it, look elsewhere, come back: that one again.
                te::AudioTrack* other = nullptr;

                for (auto* c : song::getChannels (H()))
                    if (! song::isAudioChannel (*c) && song::getChannelID (*c) != kickID)
                        other = c;

                if (other == nullptr)
                    return false;

                st.selectedChannel = song::getChannelID (*other);
                st.rememberChannel();
                st.selectedChannel = kickID;
                st.viewPattern (song::getPatterns (H()).getFirst()[song::ids::sid].toString());
                st.viewPattern (cut[song::ids::sid].toString());
                const bool remembered = st.selectedChannel == song::getChannelID (*other);
                std::cout << "    first open: " << (first ? "kick" : "WRONG") << ", after editing another: "
                          << (remembered ? other->getName() : juce::String ("WRONG")) << std::endl;
                return first && remembered;
            },
            2000 });

        //==============================================================================
        // 1.9: sidechain
        steps.push_back ({ "1.9 sidechain: peer 1 sends a tone through a 'Duck' insert with a Sidechain listening to the kick (4 hits) -> everyone has it",
            [this]
            {
                auto& e = P1();
                auto r = peer1.getAssetStore().importAudioFile (writeTestWav (testRoot.getChildFile ("duck.wav"), 4.0, 330.0, false));

                if (! r.ok())
                    return;

                song::addImportedAudio (e, r, "duck.wav", 17, 440.0);
                auto* duck = song::addInsert (e, "Duck");
                song::setRoute (e, *ch (e, "duck"), duck->itemID.toString());

                auto kicks = song::addPattern (e, "Duck kicks", 1);

                for (int step : { 0, 4, 8, 12 })
                    song::setStep (e, kicks, *ch (e, "Kick"), step, true);

                song::addClip (e, kicks[song::ids::sid].toString(), 16, 440.0);

                auto plugin = song::createEffect (e, SidechainPlugin::xmlTypeName, nullptr);
                song::putEffect (e, duck, plugin, -1);

                if (auto* sc = dynamic_cast<SidechainPlugin*> (plugin.get()))
                {
                    sc->applyPreset (2);   // hard: down to silence, back fast
                    song::setSidechainSource (e, *sc, ch (e, "Kick"));
                }
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    auto* duck = song::findInsertNamed (e, "Duck");
                    auto* kick = ch (e, "Kick");

                    if (duck == nullptr || kick == nullptr || ch (e, "duck") == nullptr)
                        return false;

                    for (auto* p : song::getInsertEffects (*duck))
                        if (auto* sc = dynamic_cast<SidechainPlugin*> (p))
                            return song::getSidechainSource (e, *sc) == kick && sc->getNumWires() == 4
                                && std::abs (sc->amount->getCurrentValue() - 1.0f) < 1.0e-4f;

                    return false;
                }) && hashesEqual() && compiledEqualEverywhere();
            },
            15000 });

        steps.push_back ({ "1.9 sidechain: ...the tone dips at every kick and comes back (measured), and passes untouched with nothing to listen to",
            {},
            [this]
            {
                auto& e = H();
                auto* kick = ch (e, "Kick");
                juce::StringArray tracks { "duck", "Duck", "Kick" };

                if (kick == nullptr || ! audioReady (e, tracks))
                    return false;

                SidechainPlugin* sc = nullptr;

                for (auto* p : song::getInsertEffects (*song::findInsertNamed (e, "Duck")))
                    if (auto* s2 = dynamic_cast<SidechainPlugin*> (p))
                        sc = s2;

                if (sc == nullptr)
                    return false;

                // The kick itself is muted so only the tone is heard: a muted track still
                // triggers a sidechain (the "ghost kick" trick).
                const bool wasMuted = kick->isMuted (false);
                kick->setMute (true);

                const double start = song::beatsToSeconds (e, 440.0);
                const double hit = song::beatsToSeconds (e, 441.0) - start;

                // Just before the second hit, just after it, and once it's back (the release is 150 ms).
                auto measure = [&] (float& before, float& after, float& back)
                {
                    const auto b = renderTracks (e, tracks, start, start + hit + 0.5);
                    before = peakIn (b, hit - 0.12, hit - 0.02);
                    after = peakIn (b, hit + 0.005, hit + 0.03);
                    back = peakIn (b, hit + 0.2, hit + 0.3);
                };

                float before = 0.0f, after = 0.0f, back = 0.0f, plainBefore = 0.0f, plainAfter = 0.0f, plainBack = 0.0f;
                measure (before, after, back);

                // Nothing to listen to: no dip (then back as it was).
                sc->setSidechainSourceID ({});
                measure (plainBefore, plainAfter, plainBack);
                song::setSidechainSource (e, *sc, kick);
                kick->setMute (wasMuted);

                duckRatio = before > 0.0f ? after / before : 1.0f;
                std::cout << "    with the kick: " << before << " before the hit, " << after << " just after (x" << duckRatio
                          << "), " << back << " once back; listening to nothing: " << plainBefore << " / " << plainAfter << std::endl;
                return before > 0.1f && duckRatio < 0.2f && back > 0.9f * before
                    && plainBefore > 0.1f && plainAfter > 0.85f * plainBefore && std::abs (before - plainBefore) < 0.1f * plainBefore;
            },
            10000, 600 });

        steps.push_back ({ "1.9 sidechain: it can't listen to anything that would loop (its own track, or a track waiting for it)",
            [this]
            {
                auto& e = H();
                auto* duck = song::findInsertNamed (e, "Duck");
                auto* kick = ch (e, "Kick");
                SidechainPlugin* sc = nullptr;
                loopRuleOk = false;

                if (duck != nullptr)
                    for (auto* p : song::getInsertEffects (*duck))
                        if (auto* s2 = dynamic_cast<SidechainPlugin*> (p))
                            sc = s2;

                if (sc == nullptr || kick == nullptr)
                    return;

                const auto sources = song::getSidechainSources (e, *sc);

                // A Sidechain on the kick channel itself (only for a moment): the Duck insert
                // waits for the kick, so the kick can't wait for it.
                auto onKick = song::createEffect (e, SidechainPlugin::xmlTypeName, nullptr);
                kick->pluginList.insertPlugin (onKick, 0, nullptr);
                const auto kickSources = song::getSidechainSources (e, *onKick);
                const bool refused = ! song::setSidechainSource (e, *onKick, duck);
                onKick->deleteFromParent();

                loopRuleOk = ! sources.contains (duck) && sources.contains (kick) && sources.contains (ch (e, "duck"))
                              && ! kickSources.contains (duck) && ! kickSources.contains (kick) && refused;
                std::cout << "    Duck may listen to " << sources.size() << " tracks (not itself); a sidechain on the kick may not listen to Duck: "
                          << (! kickSources.contains (duck) && refused ? "right" : "WRONG") << std::endl;
            },
            [this] { return loopRuleOk && hashesEqual(); },
            8000 });

        //==============================================================================
        // 1.9: playlist track groups, track mute, markers
        steps.push_back ({ "1.9 track groups: peer 2 groups tracks 17-18 (the Duck kicks and tone) and mutes the group -> everyone has it, and nothing on them plays",
            [this]
            {
                auto& e = P2();
                groupSid = song::addLaneGroup (e, 16, 2, "Duck", juce::Colours::orange);
                song::setLaneGroupMuted (e, groupSid, true);
            },
            [this]
            {
                const bool shared = everywhere ([this] (te::Edit& e)
                {
                    const auto g = song::getLaneGroupOf (e, 17);
                    return g.sid == groupSid && g.first == 16 && g.count == 2 && g.name == "Duck" && g.muted
                        && ! song::isLaneAudible (e, 16) && song::isLaneAudible (e, 15) && ! song::canGroupLanes (e, 17, 2)
                        && song::canGroupLanes (e, 18, 2);
                });

                if (! shared || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (auto* c = host.getCompiler())
                    c->compileNow();

                const auto kicks = compiled (host, "Kick").joinIntoString (" | ");
                const bool kicksGone = ! kicks.contains ("@440.000") && ! kicks.contains ("@443.000");
                const bool toneGone = compiledAudio (host, "duck").isEmpty();
                std::cout << "    muted group: kicks at 440 " << (kicksGone ? "gone" : "STILL THERE") << ", tone " << (toneGone ? "gone" : "STILL THERE") << std::endl;
                return kicksGone && toneGone;
            },
            8000 });

        steps.push_back ({ "1.9 track groups: the host unmutes the group but mutes only track 17 (the kicks) -> the tone plays again, the kicks don't",
            [this]
            {
                song::setLaneGroupMuted (H(), groupSid, false);
                song::setLaneMuted (H(), 16, true);
            },
            [this]
            {
                const bool shared = everywhere ([] (te::Edit& e)
                {
                    return song::isLaneMuted (e, 16) && ! song::isLaneAudible (e, 16) && song::isLaneAudible (e, 17)
                        && ! song::getLaneGroupOf (e, 16).muted;
                });

                if (! shared || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (auto* c = host.getCompiler())
                    c->compileNow();

                const bool kicksGone = ! compiled (host, "Kick").joinIntoString (" | ").contains ("@440.000");
                const bool toneBack = ! compiledAudio (host, "duck").isEmpty();
                std::cout << "    kicks " << (kicksGone ? "muted" : "PLAYING") << ", tone " << (toneBack ? "back" : "MISSING") << std::endl;
                return kicksGone && toneBack;
            },
            8000 });

        steps.push_back ({ "1.9 track groups: peer 1 unmutes the track and ungroups -> everything plays as before, the tracks keep their clips",
            [this]
            {
                song::setLaneMuted (P1(), 16, false);
                song::removeLaneGroup (P1(), groupSid);
            },
            [this]
            {
                const bool shared = everywhere ([] (te::Edit& e)
                {
                    bool clipsThere = false;

                    for (auto& c : song::getClips (e))
                        if ((int) c[song::ids::lane] == 16 && std::abs ((double) c[song::ids::start] - 440.0) < 1.0e-9)
                            clipsThere = true;

                    return ! song::getLaneGroupOf (e, 16).isValid() && song::isLaneAudible (e, 16) && clipsThere
                        && song::getLaneGroups (e).isEmpty();
                });

                if (! shared || ! hashesEqual() || ! compiledEqualEverywhere())
                    return false;

                if (auto* c = host.getCompiler())
                    c->compileNow();

                return compiled (host, "Kick").joinIntoString (" | ").contains ("@440.000") && ! compiledAudio (host, "duck").isEmpty();
            },
            8000 });

        steps.push_back ({ "1.9 markers: the host marks bar 111 'Drop' -> everyone sees it with the host's name; the song plays the same",
            [this]
            {
                compiledBeforeMarker = compiled (host, "Kick");
                song::addMarker (H(), 440.0, "Drop", host.getUserName());
            },
            [this]
            {
                const auto author = host.getUserName();
                const bool shared = everywhere ([author] (te::Edit& e)
                {
                    const auto markers = song::getMarkers (e);
                    return markers.size() == 1 && markers[0][song::ids::text].toString() == "Drop"
                        && std::abs (song::getMarkerBeat (markers[0]) - 440.0) < 1.0e-9 && markers[0][song::ids::author].toString() == author;
                });

                if (auto* c = host.getCompiler())
                    c->compileNow();

                return shared && hashesEqual() && compiledEqualEverywhere() && compiled (host, "Kick") == compiledBeforeMarker;
            },
            8000 });

        steps.push_back ({ "1.9 markers: peer 1 renames it 'Drop!', moves it two bars on and colours it; peer 2 adds 'Verse' at the start -> in order everywhere",
            [this]
            {
                auto m = song::getMarkers (P1()).getFirst();
                song::setMarkerText (P1(), m, "Drop!");
                song::setMarkerBeat (P1(), m, 448.0);
                song::setMarkerColour (P1(), m, juce::Colours::red);
                song::addMarker (P2(), 0.0, "Verse", peer2.getUserName());
            },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    const auto markers = song::getMarkers (e);
                    return markers.size() == 2 && markers[0][song::ids::text].toString() == "Verse"
                        && markers[1][song::ids::text].toString() == "Drop!" && std::abs (song::getMarkerBeat (markers[1]) - 448.0) < 1.0e-9
                        && song::getMarkerColour (markers[1]) == juce::Colours::red;
                }) && hashesEqual();
            },
            8000 });

        steps.push_back ({ "1.9 markers: the host deletes 'Verse' -> gone everywhere",
            [this] { song::removeMarker (H(), song::getMarkers (H()).getFirst()); },
            [this]
            {
                return everywhere ([] (te::Edit& e)
                {
                    const auto markers = song::getMarkers (e);
                    return markers.size() == 1 && markers[0][song::ids::text].toString() == "Drop!";
                }) && hashesEqual();
            },
            8000 });

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

            if (step.optional && ((step.name.contains ("VST3") && ! pluginFile.exists())
                                  || (step.name.contains ("VST2") && ! vst2File.exists())))
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

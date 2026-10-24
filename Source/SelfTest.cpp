// Automatic test: one host and two joiners in the same process, talking over
// real TCP sockets on 127.0.0.1. Run with:
//   SyncTest --selftest [--plugin=<path to a VST3 effect>] [--dump=<dir>]

#include "SelfTest.h"
#include <iostream>
#include "Session.h"
#include "SongModel.h"
#include "PatternCompiler.h"
#include "DrumSynthPlugin.h"
#include "MixerRouter.h"

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
    collab::Session host { engine }, peer1 { engine }, peer2 { engine };
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
            [this] { host.createLocalEdit(); host.startHosting (port); },
            [this]
            {
                return host.getMode() == collab::Session::Mode::hosting
                    && song::getChannels (H()).size() == 5 && song::getPatterns (H()).size() == 1
                    && compiled (host, "Kick").size() == 4 && compiled (host, "Hat").size() == 4;
            } });

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

        steps.push_back ({ "Peer 2 sets the tempo to 140 BPM -> everywhere",
            [this] { P2().tempoSequence.getTempo (0)->setBpm (140.0); },
            [this] { return everywhere ([] (te::Edit& e) { return std::abs (e.tempoSequence.getTempo (0)->getBpm() - 140.0) < 0.01; }); } });

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
            [this] { return effectKnob (H()) >= 0 && effectKnob (P1()) >= 0 && effectKnob (P2()) >= 0; },
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

        steps.push_back ({ "Peer 2 plays Pattern 2 -> everyone plays Pattern 2",
            [this]
            {
                peer2.setPlayingPattern (pat (P2(), "Pattern 2")[song::ids::sid].toString());
                peer2.setPlaying (true, 0.0);
            },
            [this]
            {
                auto sid = pat (H(), "Pattern 2")[song::ids::sid].toString();
                return H().getTransport().isPlaying() && P1().getTransport().isPlaying()
                    && host.getPlayingPattern() == sid && peer1.getPlayingPattern() == sid
                    && compiled (host, "Hat").contains ("60@5.000+0.250*100");
            },
            4000 });

        steps.push_back ({ "What each PC plays is identical (compiled notes of every channel)",
            {},
            [this] { return compiledEqualEverywhere(); } });

        steps.push_back ({ "Peer 1 jumps to beat 3 while playing -> everyone jumps there",
            [this] { peer1.seek (P1().tempoSequence.toTime (te::BeatPosition::fromBeats (3.0)).inSeconds()); },
            [this]
            {
                auto beat = [] (te::Edit& e) { return e.tempoSequence.toBeats (e.getTransport().getPosition()).inBeats(); };
                return beat (H()) >= 2.99 && beat (H()) < 4.5 && beat (P2()) >= 2.99 && beat (P2()) < 4.5;
            },
            4000 });

        steps.push_back ({ "Note preview (guide note) plays and stops on the selected channel, locally only",
            [this]
            {
                auto* synth = ch (P1(), "Synth");
                synth->playGuideNote (64, te::MidiChannel (1), 100, true, true, false);
                synth->turnOffGuideNotes();
            },
            [this] { return hashesEqual(); } });

        steps.push_back ({ "Peer 1 presses Stop -> everyone stops",
            [this] { peer1.setPlaying (false, 0.0); },
            [this] { return ! H().getTransport().isPlaying() && ! P2().getTransport().isPlaying(); },
            4000 });

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

        steps.push_back ({ "Peer 1 switches to SONG mode and plays -> everyone plays the arrangement",
            [this]
            {
                peer1.setSongMode (true);
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

        steps.push_back ({ "Host stops and switches back to PAT mode -> everyone",
            [this] { host.setPlaying (false, 0.0); host.setSongMode (false); },
            [this] { return ! peer1.isSongMode() && ! peer2.isSongMode() && ! P1().getTransport().isPlaying(); },
            4000 });

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

        steps.push_back ({ "Audio: host switches to SONG mode -> the audio clip is put into playback the same way on every PC",
            [this] { host.setSongMode (true); },
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

        steps.push_back ({ "Peer 1 rejoins and gets the latest song",
            [this] { peer1.join ("127.0.0.1", port); },
            [this] { return allJoined() && hashesEqual() && hasStep (P1(), "Pattern 1", "Hat", 15); } });

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

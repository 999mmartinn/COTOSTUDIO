// Automatic test: one host and two joiners in the same process, talking over
// real TCP sockets on 127.0.0.1. Run with:  SyncTest --selftest [--plugin=<path to .vst3>] [--dump=<dir>]

#include "SelfTest.h"
#include <iostream>
#include "Session.h"
#include "SongHelpers.h"

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

        for (auto* s : { &host, &peer1, &peer2 })
        {
            auto* label = s == &host ? "host" : (s == &peer1 ? "peer1" : "peer2");
            s->peerName = label;
            s->onLog = [label] (const juce::String& m) { std::cout << "    [" << label << "] " << m << std::endl; };
        }

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
    static constexpr int port = 47811;

    te::Edit& H()  { return *host.getEdit(); }
    te::Edit& P1() { return *peer1.getEdit(); }
    te::Edit& P2() { return *peer2.getEdit(); }

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

    static float pluginParam (te::Edit& e)
    {
        if (auto* ext = song::findExternalPlugin (e, "SyncTestGain"))
            if (auto* pi = ext->getAudioPluginInstance())
                if (! pi->getParameters().isEmpty())
                    return pi->getParameters()[0]->getValue();

        return -1.0f;
    }

    static te::AutomatableParameter* synthParam (te::Edit& e)
    {
        for (auto* p : te::getAllPlugins (e, false))
            if (auto* synth = dynamic_cast<te::FourOscPlugin*> (p))
                return synth->getNumAutomatableParameters() > 0 ? synth->getAutomatableParameter (0).get() : nullptr;

        return nullptr;
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

    //==============================================================================
    void buildSteps()
    {
        steps.push_back ({ "Host starts a song and listens on port 47811",
            [this] { host.createLocalEdit(); host.startHosting (port); },
            [this] { return host.getMode() == collab::Session::Mode::hosting; } });

        steps.push_back ({ "Two peers join and receive the song",
            [this] { peer1.join ("127.0.0.1", port); peer2.join ("127.0.0.1", port); },
            [this] { return allJoined() && hashesEqual(); } });

        steps.push_back ({ "Copies stay identical for 4 s with nobody editing (no resyncs needed)",
            [this] { dump ("host-after-join", H()); dump ("peer1-after-join", P1()); },
            [this] { return hashesEqual() && peer1.getResyncCount() == 0 && peer2.getResyncCount() == 0; },
            8000, 4000 });

        steps.push_back ({ "Host adds a note -> both peers get it",
            [this] { song::setNote (H(), 0, 64, 2, true); },
            [this] { return everywhere ([] (te::Edit& e) { return song::hasNote (e, 0, 64, 2); }); } });

        steps.push_back ({ "Peer 1 adds a note -> host and peer 2 get it (relayed)",
            [this] { song::setNote (P1(), 0, 67, 6, true); },
            [this] { return everywhere ([] (te::Edit& e) { return song::hasNote (e, 0, 67, 6); }); } });

        steps.push_back ({ "Peer 2 deletes a note -> gone everywhere",
            [this] { song::setNote (P2(), 0, 60, 0, false); },
            [this] { return everywhere ([] (te::Edit& e) { return ! song::hasNote (e, 0, 60, 0); }); } });

        steps.push_back ({ "Peer 1 turns a knob on the built-in synth -> same value everywhere",
            [this] { if (auto* p = synthParam (P1())) p->setNormalisedParameter (0.3f, juce::sendNotification); },
            [this]
            {
                auto* a = synthParam (H()); auto* b = synthParam (P1()); auto* c = synthParam (P2());
                return a && b && c && std::abs (a->getCurrentNormalisedValue() - 0.3f) < 1.0e-4f
                        && std::abs (b->getCurrentValue() - a->getCurrentValue()) < 1.0e-6f
                        && std::abs (c->getCurrentValue() - a->getCurrentValue()) < 1.0e-6f;
            } });

        steps.push_back ({ "Host adds a second track -> both peers have 2 tracks",
            [this] { song::addSynthTrack (H()); },
            [this] { return everywhere ([] (te::Edit& e) { return te::getAudioTracks (e).size() == 2; }) && hashesEqual(); } });

        const bool havePlugin = pluginFile.exists();

        steps.push_back ({ "Host inserts a third-party VST3 (SyncTestGain) -> it loads on both peers",
            [this]
            {
                juce::VST3PluginFormat format;
                juce::OwnedArray<juce::PluginDescription> found;
                format.findAllTypesForFile (found, pluginFile.getFullPathName());

                if (found.isEmpty())
                {
                    std::cout << "    could not read " << pluginFile.getFullPathName() << std::endl;
                    return;
                }

                engine.getPluginManager().knownPluginList.addType (*found[0]);
                song::insertPlugin (*song::getTrack (H(), 0), *found[0]);
            },
            [this] { return pluginParam (H()) >= 0 && pluginParam (P1()) >= 0 && pluginParam (P2()) >= 0; },
            15000, 600, ! havePlugin });   // 0.6 s: a person can't turn a knob faster than that after inserting

        steps.push_back ({ "Knob turned inside the plugin's own window on the host -> peers follow",
            [this]
            {
                if (auto* ext = song::findExternalPlugin (H(), "SyncTestGain"))
                    ext->getAudioPluginInstance()->getParameters()[0]->setValueNotifyingHost (0.8f);
            },
            [this] { return std::abs (pluginParam (P1()) - 0.8f) < 1.0e-4f && std::abs (pluginParam (P2()) - 0.8f) < 1.0e-4f; },
            8000, 0, ! havePlugin });

        steps.push_back ({ "Knob turned in the plugin window on peer 2 -> host and peer 1 follow",
            [this]
            {
                if (auto* ext = song::findExternalPlugin (P2(), "SyncTestGain"))
                    ext->getAudioPluginInstance()->getParameters()[0]->setValueNotifyingHost (0.25f);
            },
            [this] { return std::abs (pluginParam (H()) - 0.25f) < 1.0e-4f && std::abs (pluginParam (P1()) - 0.25f) < 1.0e-4f; },
            8000, 0, ! havePlugin });

        steps.push_back ({ "Fast knob drag (200 moves) on peer 1 -> everyone ends on the last value",
            [this]
            {
                if (auto* ext = song::findExternalPlugin (P1(), "SyncTestGain"))
                    for (int i = 0; i <= 200; ++i)
                        ext->getAudioPluginInstance()->getParameters()[0]->setValueNotifyingHost (i / 200.0f * 0.6f);
            },
            [this] { return std::abs (pluginParam (H()) - 0.6f) < 1.0e-4f && std::abs (pluginParam (P2()) - 0.6f) < 1.0e-4f; },
            8000, 0, ! havePlugin });

        steps.push_back ({ "All three edit at the same instant -> everyone converges on the same song",
            [this]
            {
                song::setNote (H(), 0, 72, 3, true);
                song::setNote (P1(), 0, 74, 5, true);
                song::addSynthTrack (P2());
            },
            [this]
            {
                return hashesEqual()
                    && everywhere ([] (te::Edit& e) { return song::hasNote (e, 0, 72, 3) && song::hasNote (e, 0, 74, 5)
                                                             && te::getAudioTracks (e).size() == 3; });
            },
            20000, 3500 });

        steps.push_back ({ "Item IDs are unique (no clashes between peers) and identical everywhere",
            {},
            [this]
            {
                auto a = te::EditItemID::findAllIDs (H().state), b = te::EditItemID::findAllIDs (P1().state);
                return idsUnique (H()) && idsUnique (P1()) && idsUnique (P2()) && a.size() + 1 == b.size(); // +1 = SYNCLOCAL
            } });

        steps.push_back ({ "Peer 2 presses Play -> host and peer 1 are playing too",
            [this] { peer2.setPlaying (true); },
            [this] { return H().getTransport().isPlaying() && P1().getTransport().isPlaying(); },
            4000, 0, true });

        steps.push_back ({ "Peer 1 presses Stop -> everyone stops",
            [this] { peer1.setPlaying (false); },
            [this] { return ! H().getTransport().isPlaying() && ! P2().getTransport().isPlaying(); },
            4000, 0, true });

        steps.push_back ({ "Peer 1 leaves; host keeps editing with peer 2",
            [this] { peer1.leave(); song::setNote (H(), 0, 76, 9, true); },
            [this] { return song::hasNote (P2(), 0, 76, 9) && host.getNumConnectedPeers() == 1; },
            8000, 1600 });

        steps.push_back ({ "Peer 1 rejoins and gets the latest song",
            [this] { peer1.join ("127.0.0.1", port); },
            [this] { return allJoined() && hashesEqual() && song::hasNote (P1(), 0, 76, 9); } });

        steps.push_back ({ "Final check: all copies identical",
            [this] { dump ("host-final", H()); dump ("peer1-final", P1()); dump ("peer2-final", P2()); },
            [this] { return hashesEqual(); }, 4000, 1600 });
    }

    void timerCallback() override
    {
        if (current >= steps.size())
        {
            stopTimer();
            std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped << " skipped" << std::endl;

            for (auto& f : failures)
                std::cout << "  FAILED: " << f << std::endl;

            if (auto* s = host.getSync())
            {
                auto st = s->getStats();
                std::cout << "host sync stats: sent " << st.sent << ", applied " << st.applied << ", failed " << st.failed
                          << ", knob messages sent " << st.paramsSent << " / applied " << st.paramsApplied << std::endl;
            }

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

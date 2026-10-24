#pragma once

#include "SongModel.h"

/**
    Applies the shared routing (each track's routeTo = an insert's ID, or master)
    to the engine on this PC.

    The engine itself stores routing as "track number N", which shifts whenever
    someone adds or removes a track, so that setting is kept local (never sent)
    and rebuilt here from the IDs. Anything dangling or looping goes to master.
*/
class MixerRouter  : private juce::ValueTree::Listener,
                     private juce::AsyncUpdater,
                     private juce::Timer
{
public:
    explicit MixerRouter (te::Edit&);
    ~MixerRouter() override;

    /** Brings the engine's routing up to date right away. */
    void applyNow();

private:
    te::Edit& edit;
    bool applying = false;

    void handleAsyncUpdate() override       { applyNow(); }
    void timerCallback() override           { applyNow(); }   // safety net (track renumbering)
    void changed()                          { if (! applying) triggerAsyncUpdate(); }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& id) override
    {
        if (id == song::ids::routeTo || id == song::ids::insert)
            changed();
    }

    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree&) override        { if (p == edit.state) changed(); }
    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree&, int) override { if (p == edit.state) changed(); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override         { if (p == edit.state) changed(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerRouter)
};

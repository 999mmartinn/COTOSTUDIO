#pragma once

#include "SongModel.h"

/**
    Applies the shared mixer layout to the engine on this PC:
      - routing: each track's routeTo (an insert's ID, or master). The engine stores
        routing as "track number N", which shifts whenever someone adds or removes a
        track, so that setting is kept local (never sent) and rebuilt here from the IDs;
      - sends (SONGMODEL/SENDS): an engine "aux send" on the source insert and an
        "aux return" on the target insert, built here and marked generated (never sent);
      - solo, which is personal: tracks you don't want to hear get no output.
    Anything dangling or looping goes to master / is left out.
*/
class MixerRouter  : private juce::ValueTree::Listener,
                     private juce::AsyncUpdater,
                     private juce::Timer
{
public:
    explicit MixerRouter (te::Edit&);
    ~MixerRouter() override;

    /** What this PC listens to: soloed channels and inserts (track IDs). */
    struct Solo
    {
        juce::StringArray channels, inserts;
        bool isEmpty() const noexcept               { return channels.isEmpty() && inserts.isEmpty(); }
        bool operator== (const Solo& o) const       { return channels == o.channels && inserts == o.inserts; }
        bool operator!= (const Solo& o) const       { return ! operator== (o); }
        juce::String toString() const;              // "c:id,c:id,i:id" (sent along while sharing)
        static Solo fromString (const juce::String&);
    };

    void setSolo (const Solo&);
    const Solo& getSolo() const noexcept            { return solo; }
    /** True if this track can't be heard right now because something else is soloed. */
    bool isSilencedBySolo (const juce::String& trackID) const   { return silenced.contains (trackID); }

    /** Brings the engine up to date right away. */
    void applyNow();

    /** Bus each insert receives sends on (for tests and the UI): -1 = none. */
    int getReturnBus (const juce::String& insertID) const;

private:
    te::Edit& edit;
    bool applying = false;
    Solo solo;
    juce::StringArray silenced;
    std::map<juce::String, int> returnBuses;

    void computeSilenced();
    void applySends();

    void handleAsyncUpdate() override       { applyNow(); }
    void timerCallback() override           { applyNow(); }   // safety net (track renumbering)
    void changed()                          { if (! applying) triggerAsyncUpdate(); }

    static bool isAboutSends (const juce::ValueTree& v)
    {
        return v.hasType (song::ids::SENDS) || v.hasType (song::ids::SEND);
    }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (id == song::ids::routeTo || id == song::ids::insert || v.hasType (song::ids::SEND))
            changed();
    }

    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree& c) override
    {
        if (p == edit.state || isAboutSends (p) || isAboutSends (c) || c.hasType (song::ids::SONGMODEL))
            changed();
    }

    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree& c, int) override
    {
        if (p == edit.state || isAboutSends (p) || isAboutSends (c) || c.hasType (song::ids::SONGMODEL))
            changed();
    }

    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override         { if (p == edit.state) changed(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerRouter)
};

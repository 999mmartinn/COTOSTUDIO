#pragma once

#include "SongModel.h"

class PatternCompiler;
class MixerRouter;

/**
    "Bounce to audio" (FL's "Render as audio clip", Pro Tools' "Bounce" / "Commit"):
    turns one playlist clip - a pattern, or an audio clip with its fades, volume, pitch
    and speed - into plain audio.

    Like the Exporter it works on a private copy of the song made when you start it, in
    which only that clip plays (automation clips still play, they shape the sound). Each
    channel the clip plays becomes its own file, rendered the way the channel sounds
    before the mixer (its instrument, its own effects, volume and pan), so the new audio
    clips go through the same mixer inserts and sound the same. The files are trimmed
    after the sound ends (reverb and release tails are kept) and added to the song's
    audio, which sends them to everyone. Uses this PC's plugins: a plugin that isn't
    installed here renders silent (that channel is left out).
*/
class Bouncer
{
public:
    Bouncer (te::Engine&, AssetStore&);
    ~Bouncer();

    struct Part
    {
        juce::String channelID, channelName;   // the channel it came from
        AssetStore::ImportResult audio;        // the new file, in the store
        double seconds = 0.0;                  // how long the sound lasts (with its tail)
    };

    struct Result
    {
        juce::String error, clipSid;
        double startBeat = 0.0;
        std::vector<Part> parts;
        int silentChannels = 0;
        bool ok() const noexcept               { return error.isEmpty() && ! parts.empty(); }
    };

    /** Starts bouncing a clip of this song state (from EditSync::createSnapshot). Returns an
        error text, or "" if it started. onDone runs on the message thread. */
    juce::String start (const juce::ValueTree& songState, const juce::String& clipSid, std::function<void (const Result&)> onDone);
    bool isRunning() const noexcept            { return running; }

    /** Puts the bounced audio into the song in place of the clip (message thread): one new
        audio channel per part, routed like the channel it came from, its clip where the
        original started (the first on the same track, the others on free tracks below),
        and removes the original clip. Returns the new clips (none if the original clip
        is gone meanwhile). */
    static juce::Array<juce::ValueTree> apply (te::Edit&, const Result&);

private:
    te::Engine& engine;
    AssetStore& assets;
    std::unique_ptr<te::Edit> copy;
    std::unique_ptr<PatternCompiler> compiler;
    std::unique_ptr<MixerRouter> router;
    std::unique_ptr<te::RenderQueue> queue;
    juce::ThreadPool pool { 1 };
    bool running = false;

    struct Planned { juce::String channelID, channelName; juce::File file; };
    std::vector<Planned> planned;
    Result result;
    double clipSeconds = 0.0;
    std::function<void (const Result&)> onDone;

    void rendersFinished();
    void finish();
    void cleanUp();

    JUCE_DECLARE_WEAK_REFERENCEABLE (Bouncer)
    JUCE_DECLARE_NON_COPYABLE (Bouncer)
};

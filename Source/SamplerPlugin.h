#pragma once

#include "CotoFx.h"

//==============================================================================
/**
    The channel rack's sampler (like FL's): plays one audio file - a drum hit,
    a one-shot, a loop - each time a note starts.
      - The whole sound plays, however short the step is (a one-shot), unless the
        envelope is on: then it follows attack / hold / decay / sustain and fades out
        (release) when the note ends, like FL's channel envelope. "Stop at note end"
        (no envelope needed) cuts it when the note ends, with a 10 ms fade: an 808
        plays only as long as its note.
      - A note's distance from C5 (60) shifts it in semitones, like speeding a
        record up (higher = shorter), plus the Tune and Fine knobs.
      - Start / End trim the sound, Fade in / out soften its ends, Reverse plays it
        backwards (the trim and the fades then apply to the reversed sound).
      - A filter (low / high / band pass) with cutoff and resonance.
      - "Cut itself" stops the previous hit when a new one starts (for 808s).
      - Each note's own pan, fine pitch and release velocity (FL's note properties, set
        in the piano roll) change that note only.
    All of it is in the "Edit sound" window (right-click the channel in the rack); the
    knobs are stored in the song, so they sync and can be automated.

    Which file it plays is part of the song (its fingerprint, "asset", synced
    like everything else). Each PC loads its own copy of that file (see
    PatternCompiler), so on a PC where the file hasn't arrived yet the channel
    is silent until it does.
*/
class SamplerPlugin  : public CotoFxPlugin
{
public:
    explicit SamplerPlugin (te::PluginCreationInfo);
    ~SamplerPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Sampler"); }
    static const char* xmlTypeName;
    static const juce::Identifier assetID, cutID, noteLengthID;

    juce::String getName() const override               { return "Sampler"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Sampler"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool isSynth() override                             { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    double getTailLength() const override
    {
        // The whole sound (a one-shot), plus the release when the envelope is on
        // (exports end this much after the last note).
        return sampleSeconds.load() + (envOnParam->getCurrentValue() >= 0.5f ? releaseParam->getCurrentValue() * 0.001 : 0.0);
    }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    /** The file this channel plays (its fingerprint in the song's audio). */
    juce::String getAsset() const                       { return state[assetID].toString(); }

    /** Loads this PC's copy of the sound (message thread). If the file isn't here yet
        the channel stays silent; call again once it is. Does nothing if already loaded. */
    void loadSample (const juce::String& hash, const juce::File& file);

    /** The fingerprint of the sound loaded right now ("" = none), and its length. */
    juce::String getLoadedHash() const                  { return loadedFromFile ? loadedHash : juce::String(); }
    double getSampleSeconds() const                     { return sampleSeconds.load(); }

    /** The loaded sound's outline for the editor (message thread): pairs of min / max,
        `peakBins` of them across the whole file. Empty = no sound. */
    static constexpr int peakBins = 1024;
    const std::vector<float>& getPeaks() const          { return peaks; }

    static constexpr int maxVoices = 16;
    /** Where each voice is playing, 0..1 across the (possibly reversed) sound; -1 = idle. */
    float getVoicePosition (int i) const                { return voicePositions[(size_t) i].load(); }

    juce::CachedValue<bool> cutValue, noteLengthValue;   // Cut itself; Stop at note end
    te::AutomatableParameter *tuneParam, *fineParam, *levelParam;
    te::AutomatableParameter *startParam, *endParam, *fadeInParam, *fadeOutParam, *reverseParam;
    te::AutomatableParameter *envOnParam, *attackParam, *holdParam, *decayParam, *sustainParam, *releaseParam;
    te::AutomatableParameter *filterTypeParam, *cutoffParam, *resonanceParam;

    static constexpr double maxSeconds = 300.0;   // longer files are cut here (memory)

    //==============================================================================
    /** The trimmed part of the file, in its samples: [first, last). Reverse flips the knobs
        (they are in the order the sound plays). Shared with the self-test. */
    static juce::Range<double> regionOf (int length, float start, float end, bool reverse)
    {
        float lo = juce::jlimit (0.0f, 1.0f, juce::jmin (start, end));
        float hi = juce::jlimit (0.0f, 1.0f, juce::jmax (start, end));

        if (reverse)
            std::tie (lo, hi) = std::make_pair (1.0f - hi, 1.0f - lo);

        const double first = std::floor (lo * (double) length);
        const double last = juce::jlimit (juce::jmin ((double) length, first + 2.0), (double) length, std::floor (hi * (double) length));
        return { first, last };
    }

private:
    struct Sample
    {
        juce::AudioBuffer<float> audio;
        double rate = 44100.0;
    };

    enum class Stage { attack, hold, decay, sustain, release };

    struct Voice
    {
        bool active = false, released = false, useEnvelope = false;
        int note = -1;
        double position = 0.0, startPosition = 0.0, step = 1.0;   // in the file's samples (step < 0 = backwards)
        float gain = 1.0f, fade = 1.0f, fadeStep = 0.0f;           // velocity; the "cut itself" fade
        float gainL = 1.0f, gainR = 1.0f;                           // the note's pan
        Stage stage = Stage::attack;
        float env = 1.0f, releaseStep = 0.0f;
        int holdLeft = 0;
    };

    /** The knobs, read once per block. */
    struct Settings
    {
        juce::Range<double> region;
        double fadeIn = 0.0, fadeOut = 0.0;     // in the file's samples
        bool reverse = false, envelope = false, stopAtNoteEnd = false;
        float tune = 0.0f, level = 1.0f, sustain = 1.0f;
        float attackStep = 1.0f, decayStep = 1.0f, releaseSamples = 1.0f;
        int holdSamples = 0, filterType = 0;
        float cutoff = 20000.0f, resonance = 0.0f;
    };

    std::unique_ptr<Sample> sample;          // swapped under sampleLock
    juce::CriticalSection sampleLock;
    std::array<Voice, maxVoices> voices;
    std::array<std::atomic<float>, maxVoices> voicePositions;
    int nextVoice = 0;
    double sampleRate = 44100.0;
    juce::String loadedHash;
    bool loadedFromFile = false;
    std::atomic<double> sampleSeconds { 0.0 };
    std::vector<float> peaks;

    // The filter (a TPT state-variable filter per side, after the voices).
    float ic1[2] {}, ic2[2] {}, smoothedCutoff = 20000.0f, fa1 = 0.0f, fa2 = 0.0f, fa3 = 0.0f, fk = 2.0f;
    int lastFilterType = 0, coefficientCountdown = 0;
    void updateFilterCoefficients (float resonance);

    Settings readSettings (const Sample&) const;
    void startVoice (const Sample&, const Settings&, int noteNumber, float velocity, bool cut,
                     float pan = 0.0f, float fineCents = 0.0f);
    void releaseNote (int noteNumber, const Settings&, int releaseVelocity = 0);
    float nextEnvelope (Voice&, const Settings&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerPlugin)
};

#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

//==============================================================================
/**
    The channel rack's sampler (like FL's): plays one audio file - a drum hit,
    a one-shot, a loop - each time a note starts.
      - The whole sound plays, however short the step is (a one-shot).
      - A note's distance from C5 (60) shifts it in semitones, like speeding a
        record up (higher = shorter), plus the Tune knob.
      - "Cut itself" stops the previous hit when a new one starts (for 808s).

    Which file it plays is part of the song (its fingerprint, "asset", synced
    like everything else). Each PC loads its own copy of that file (see
    PatternCompiler), so on a PC where the file hasn't arrived yet the channel
    is silent until it does.
*/
class SamplerPlugin  : public te::Plugin
{
public:
    explicit SamplerPlugin (te::PluginCreationInfo);
    ~SamplerPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Sampler"); }
    static const char* xmlTypeName;
    static const juce::Identifier assetID, cutID;

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
    double getTailLength() const override               { return sampleSeconds.load(); }

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

    juce::CachedValue<float> tuneValue, levelValue;
    juce::CachedValue<bool> cutValue;
    te::AutomatableParameter::Ptr tuneParam, levelParam;

    static constexpr double maxSeconds = 300.0;   // longer files are cut here (memory)

private:
    struct Sample
    {
        juce::AudioBuffer<float> audio;
        double rate = 44100.0;
    };

    struct Voice
    {
        bool active = false;
        double position = 0.0, step = 1.0;   // in the file's samples
        float gain = 1.0f, fade = 1.0f, fadeStep = 0.0f;
    };

    static constexpr int maxVoices = 16;
    std::unique_ptr<Sample> sample;          // swapped under sampleLock
    juce::CriticalSection sampleLock;
    std::array<Voice, maxVoices> voices;
    int nextVoice = 0;
    double sampleRate = 44100.0;
    juce::String loadedHash;
    bool loadedFromFile = false;
    std::atomic<double> sampleSeconds { 0.0 };

    void startVoice (const Sample&, int noteNumber, float velocity, float tune, bool cut);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerPlugin)
};

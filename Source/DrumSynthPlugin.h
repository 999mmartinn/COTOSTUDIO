#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

//==============================================================================
/**
    A tiny built-in drum instrument made purely from synthesis (no sample files),
    so a new song has a playable kit on every PC without sharing any files.

    One instance plays one sound (Kick, Clap, Hat, Snare). MIDI notes trigger it;
    the note's distance from C5 (60) shifts the pitch in semitones and the
    velocity sets the level. All settings live in the song data, so they sync.
*/
class DrumSynthPlugin  : public te::Plugin
{
public:
    explicit DrumSynthPlugin (te::PluginCreationInfo);
    ~DrumSynthPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Drum Synth"); }
    static const char* xmlTypeName;

    enum Sound { kick = 0, clap, hat, snare, numSounds };
    static juce::String getSoundName (int sound);

    juce::String getName() const override               { return getSoundName (soundValue.get()) + " (Drum Synth)"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Drums"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool isSynth() override                             { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    double getTailLength() const override               { return 1.0; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<int> soundValue;
    juce::CachedValue<float> tuneValue, decayValue, levelValue;
    te::AutomatableParameter::Ptr soundParam, tuneParam, decayParam, levelParam;

private:
    struct Voice
    {
        bool active = false;
        double time = 0.0;          // seconds since the hit
        float velocity = 1.0f, pitchRatio = 1.0f;
        double phase = 0.0;
        float hp = 0.0f, lastNoise = 0.0f;
        juce::Random random;
    };

    static constexpr int maxVoices = 8;
    std::array<Voice, maxVoices> voices;
    double sampleRate = 44100.0;
    int nextVoice = 0;

    void startVoice (int noteNumber, float velocity);
    float renderVoiceSample (Voice&, int sound, float tune, float decay);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSynthPlugin)
};

#pragma once

#include "CotoFx.h"

//==============================================================================
/**
    COTOSTUDIO's Sidechain (a built-in mixer effect): makes a sound duck - get quieter
    for a moment - every time another sound hits. The classic use: put it on the bass
    (or pads, or the whole beat minus drums) and let it listen to the kick, so the kick
    always cuts through and the music "pumps" with it.

    What it listens to is the plugin's sidechain source (any channel or insert, chosen
    in its window); the engine feeds that track's sound into inputs 3 and 4.

    Each hit of the trigger (a sudden jump in its level, louder than the sensitivity)
    starts one dip of the same shape - like a kick-triggered "pump" - so a long, booming
    kick tail doesn't keep the sound down. The knobs:
      - Amount: how much quieter the sound gets (100% = silent);
      - Sensitivity: how loud a hit must be to count;
      - Attack: how fast it dips;
      - Release: how long it takes to come all the way back.
    With nothing chosen to listen to, the sound passes through untouched.
*/
class SidechainPlugin  : public CotoFxPlugin
{
public:
    explicit SidechainPlugin (te::PluginCreationInfo);
    ~SidechainPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Sidechain"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Sidechain"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Sidechain"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override;
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }
    double getTailLength() const override               { return 0.0; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    te::AutomatableParameter *amount, *sensitivity, *attack, *release;

    /** The meters for the window (written by the audio thread, read and reset by the window). */
    std::atomic<float> triggerPeak { 0.0f };   // the loudest the trigger was (gain)
    std::atomic<float> lowestGain { 1.0f };    // the most it ducked (gain)
    std::atomic<bool> hearingTrigger { false };   // a trigger signal arrives at all

    /** Ready-made settings: 0 = gentle, 1 = pumping (EDM), 2 = hard (trap / 808). */
    void applyPreset (int which);

private:
    double sampleRate = 44100.0;
    float fast = 0.0f, slow = 0.0f;     // the trigger's level: quick and slow followers (a hit = fast jumps above slow)
    bool armed = true;                  // ready for the next hit
    int sinceHit = 1 << 30;             // samples since the last hit
    float gain = 1.0f, dipFrom = 1.0f;  // now, and where the current dip started

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidechainPlugin)
};

#pragma once

#include "CotoFx.h"
#include "CotoDelay.h"

//==============================================================================
/**
    COTOSTUDIO's Delay (a built-in mixer effect), in the spirit of FL's Fruity Delay 3:
    mono / stereo / ping pong echoes, tempo sync, keep pitch, a filter, lo-fi, diffusion
    and distortion on every repeat, modulation, and tone / wet / dry on the output.
*/
class DelayPlugin  : public CotoFxPlugin,
                     private juce::Timer
{
public:
    explicit DelayPlugin (te::PluginCreationInfo);
    ~DelayPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Delay"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Delay"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Delay"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }   // the echoes go on
    double getTailLength() const override               { return 8.0; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override                               { processor.reset(); }
    void applyToBuffer (const te::PluginRenderContext&) override;

    dly::Settings getSettings() const;
    double getBpm() const                               { return bpm.load(); }

    te::AutomatableParameter *model, *input, *sync, *division, *timeMs, *keepPitch, *smoothing, *offset, *spread;
    te::AutomatableParameter *feedback, *filterType, *cutoff, *resonance, *sampleRate, *bits;
    te::AutomatableParameter *modRate, *modTime, *modCutoff, *diffusion, *diffusionSpread;
    te::AutomatableParameter *distType, *distKnee, *distSymmetry, *distLevel;
    te::AutomatableParameter *wet, *tone, *dry;

private:
    dly::Processor processor;
    double rate = 44100.0;
    std::atomic<double> bpm { 120.0 };

    void readTempo();
    void timerCallback() override       { readTempo(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayPlugin)
};

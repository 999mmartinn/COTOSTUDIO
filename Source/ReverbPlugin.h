#pragma once

#include "CotoFx.h"
#include "CotoReverb.h"

//==============================================================================
/**
    COTOSTUDIO's Reverb (a built-in mixer effect), in the spirit of FL's Fruity Reeverb 2:
    low / high cut, pre-delay (also in note values), room size, diffusion, modulation,
    decay up to 20 s with a bass multiplier and crossover, high damping, and dry / early
    reflections / wet / stereo separation, on the mid or the side.
*/
class ReverbPlugin  : public CotoFxPlugin,
                      private juce::Timer
{
public:
    explicit ReverbPlugin (te::PluginCreationInfo);
    ~ReverbPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Reverb"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Reverb"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Reverb"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }   // the tail goes on
    double getTailLength() const override               { return juce::jmin (40.0, 1.5 * decay->getCurrentValue() * juce::jmax (1.0f, bass->getCurrentValue()) + 2.5); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override                               { processor.reset(); }
    void applyToBuffer (const te::PluginRenderContext&) override;

    rvb::Settings getSettings() const;

    te::AutomatableParameter *lowCut, *highCut, *tempo, *predelay, *predelayDivision, *size, *diffusion, *mod, *modSpeed;
    te::AutomatableParameter *decay, *bass, *crossover, *damp, *dry, *early, *wet, *separation, *side;

private:
    rvb::Processor processor;
    std::atomic<double> bpm { 120.0 };

    void readTempo();
    void timerCallback() override       { readTempo(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbPlugin)
};

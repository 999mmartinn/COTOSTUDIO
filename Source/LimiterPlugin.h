#pragma once

#include "CotoFx.h"
#include "CotoLimiter.h"

//==============================================================================
/**
    COTOSTUDIO's Limiter (a built-in mixer effect): compressor, look-ahead limiter,
    noise gate and saturation, in the spirit of FL's Fruity Limiter.

    The limiter's attack is also its look-ahead, so it adds that much latency; the
    engine compensates it. When the attack knob settles on a new value, playback is
    rebuilt once so the compensation follows (like a VST3 changing its latency).
*/
class LimiterPlugin  : public CotoFxPlugin,
                       private juce::Timer
{
public:
    explicit LimiterPlugin (te::PluginCreationInfo);
    ~LimiterPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Limiter"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Limiter"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Limiter"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }
    double getTailLength() const override               { return 0.5; }
    double getLatencySeconds() override                 { return latencySamples.load() / sampleRate; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override                               { processor.reset(); }
    void applyToBuffer (const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    lim::Settings getSettings() const;

    // Compressor
    te::AutomatableParameter *compGain, *compThresh, *compRatio, *compKnee, *compAttack, *compRelease, *compCurve, *compSustain;
    // Limiter
    te::AutomatableParameter *limGain, *ceiling, *limAttack, *limRelease, *limCurve, *limSustain;
    // Gate and saturation
    te::AutomatableParameter *gateGain, *gateThresh, *gateRelease, *saturation;

    /** Display frames for the editor (only collected while one is open). */
    std::atomic<bool> displayWanted { false };
    juce::AbstractFifo frameFifo { 512 };
    std::array<lim::Frame, 512> frames;

    double getSampleRate() const                        { return sampleRate; }

private:
    lim::Processor processor;
    double sampleRate = 44100.0;
    std::atomic<int> latencySamples { 0 };
    int pendingLatency = -1;

    int lookaheadFromKnob() const;
    void timerCallback() override;      // follows the attack knob (message thread)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterPlugin)
};

#pragma once

#include "CotoFx.h"

//==============================================================================
/**
    Four more of COTOSTUDIO's own mixer effects (1.12), made for vocal chains and for
    bringing FL Studio templates over (each takes the settings of the FL effect it
    stands in for when a project is imported):

      - Compressor:  threshold, ratio, gain, attack, release and a type (how soft the knee
                     is; the RMS types react to loudness rather than peaks).
      - Enhancer:    one knob that makes a sound fuller and louder, in four flavours (A-D):
                     three bands, each compressed upwards, then a safety limiter.
      - Flanger:     several swept short delays (the order = how many), with depth, speed,
                     delay, stereo spread, cross feedback, and dry / wet that can be
                     turned negative (phase inverted) for the hollow flanger sound.
      - Filter:      a resonant state-variable filter (1-3 stages) whose low / band / high
                     outputs are mixed with three sliders, with input and output volume
                     and pan - a "lo-fi / telephone / muffled" tool for adlibs.
*/
namespace morefx
{
    /** A one-pole envelope follower with separate attack and release. */
    struct Follower
    {
        float value = 0.0f, attack = 0.0f, release = 0.0f;

        void setTimes (double sampleRate, float attackMs, float releaseMs)
        {
            auto coefficient = [sampleRate] (float ms) { return ms <= 0.0f ? 0.0f : std::exp (-1.0f / (float) (ms * 0.001 * sampleRate)); };
            attack = coefficient (attackMs);
            release = coefficient (releaseMs);
        }

        float process (float x) noexcept
        {
            value = x > value ? x + (value - x) * attack : x + (value - x) * release;
            JUCE_UNDENORMALISE (value);
            return value;
        }
    };

    /** Gain reduction (dB, <= 0) of a compressor at an input level, with a soft knee. */
    inline float compressDb (float inDb, float threshold, float ratio, float knee) noexcept
    {
        const float over = inDb - threshold;
        const float slope = 1.0f / juce::jmax (1.0f, ratio) - 1.0f;

        if (knee > 0.0f && std::abs (over) <= knee * 0.5f)
        {
            const float x = over + knee * 0.5f;
            return slope * x * x / (2.0f * knee);
        }

        return over > 0.0f ? slope * over : 0.0f;
    }

    /** A TPT state-variable filter giving low, band and high pass at once. */
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

        void set (double sampleRate, float cutoff, float q)
        {
            g = std::tan (juce::MathConstants<float>::pi * juce::jlimit (10.0f, (float) (sampleRate * 0.49), cutoff) / (float) sampleRate);
            k = 1.0f / juce::jmax (0.05f, q);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        void process (float x, float& low, float& band, float& high) noexcept
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            JUCE_UNDENORMALISE (ic1);
            JUCE_UNDENORMALISE (ic2);
            low = v2;
            band = v1;
            high = x - k * v1 - v2;
        }

        void reset()    { ic1 = ic2 = 0.0f; }
    };
}

//==============================================================================
class CompressorFxPlugin  : public CotoFxPlugin
{
public:
    explicit CompressorFxPlugin (te::PluginCreationInfo);
    ~CompressorFxPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Compressor"); }
    static const char* xmlTypeName;
    static juce::StringArray typeNames();

    juce::String getName() const override               { return "Compressor"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Comp"; }
    juce::String getSelectableDescription() override    { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }

    void initialise (const te::PluginInitialisationInfo& info) override   { sampleRate = info.sampleRate; reset(); }
    void deinitialise() override                        {}
    void reset() override                               { follower.value = 0.0f; }
    void applyToBuffer (const te::PluginRenderContext&) override;

    te::AutomatableParameter *threshold, *ratio, *gain, *attack, *release, *type;
    std::atomic<float> lastReduction { 0.0f };   // dB, for the window

private:
    double sampleRate = 44100.0;
    morefx::Follower follower;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressorFxPlugin)
};

//==============================================================================
class EnhancerPlugin  : public CotoFxPlugin
{
public:
    explicit EnhancerPlugin (te::PluginCreationInfo);
    ~EnhancerPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Enhancer"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Enhancer"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Enhance"; }
    juce::String getSelectableDescription() override    { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    te::AutomatableParameter *mode, *amount;

private:
    double sampleRate = 44100.0;
    float lowA[2] {}, lowB[2] {}, highA[2] {}, highB[2] {};   // band splitting (two one-pole stages each)
    morefx::Follower bands[3], limiter;
    float smoothedAmount = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnhancerPlugin)
};

//==============================================================================
class FlangerPlugin  : public CotoFxPlugin
{
public:
    explicit FlangerPlugin (te::PluginCreationInfo);
    ~FlangerPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Flanger"); }
    static const char* xmlTypeName;
    static constexpr int maxOrder = 8;

    juce::String getName() const override               { return "Flanger"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Flanger"; }
    juce::String getSelectableDescription() override    { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    double getTailLength() const override               { return 0.1; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    te::AutomatableParameter *order, *depth, *speed, *delay, *spread, *cross, *dry, *wet;

private:
    double sampleRate = 44100.0;
    juce::AudioBuffer<float> line;   // 2 channels of delay
    int writePos = 0;
    double phase = 0.0;
    float feedback[2] {};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlangerPlugin)
};

//==============================================================================
class FilterFxPlugin  : public CotoFxPlugin
{
public:
    explicit FilterFxPlugin (te::PluginCreationInfo);
    ~FilterFxPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Filter"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Filter"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Filter"; }
    juce::String getSelectableDescription() override    { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }

    void initialise (const te::PluginInitialisationInfo& info) override   { sampleRate = info.sampleRate; reset(); }
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    te::AutomatableParameter *inVol, *stages, *cutoff, *resonance, *lowMix, *bandMix, *highMix, *pan, *outVol;

private:
    double sampleRate = 44100.0;
    morefx::Svf filters[2][3];
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterFxPlugin)
};

//==============================================================================
/** The window of the four: a title line of help, rows of knobs and choice buttons. */
class MoreFxEditor  : public juce::Component
{
public:
    explicit MoreFxEditor (CotoFxPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::String title;
        juce::Colour colour;
        juce::Array<juce::Component*> items;
        juce::Rectangle<int> area;
    };

    CotoFxPlugin& plugin;
    juce::OwnedArray<juce::Component> controls;
    std::vector<Row> rows;
    juce::String help;
    int choiceButtons = 0;

    void addKnobs (const juce::String& title, juce::Colour, std::initializer_list<std::pair<te::AutomatableParameter*, juce::String>>,
                   std::initializer_list<te::AutomatableParameter*> bipolar = {});
    void addChoice (const juce::String& title, juce::Colour, te::AutomatableParameter&, const juce::StringArray& names);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MoreFxEditor)
};

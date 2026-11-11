#pragma once

#include "MoreFx.h"

//==============================================================================
/**
    Five more of COTOSTUDIO's own mixer effects (2.3), to finish a vocal chain and a
    master without third-party plugins:

      - De-esser:      turns the harsh "s" and "sh" sounds down. Watches the highs above
                       a frequency; when they pass the threshold it turns down either the
                       whole sound (Wide) or only those highs (Split). Listen plays just
                       what it watches, to find the right frequency.
      - Multiband:     a compressor with three bands (low / mid / high), each with its own
                       threshold, ratio and gain, and two movable crossovers (24 dB per
                       octave; the bands add up to the input's level when nothing is compressed).
      - Distortion:    drive into a warm, hard or folding shape, a tone (low pass) knob,
                       a bit crusher (bits and sample-rate reduction) and dry / wet mix.
      - Stereo shaper: width (0 = mono, 200% = extra wide), a short delay on one side
                       (Haas: makes a mono voice wide), bass kept in the middle, pan,
                       and swapping / flipping the two sides.
      - Loudness meter: changes nothing; shows the loudness the streaming services
                       measure (LUFS: momentary, short-term, integrated) and the peak.
*/
namespace morefx
{
    /** A 24 dB / octave (Linkwitz-Riley) split: low and high add up to the input with a
        flat level. */
    struct Crossover
    {
        Svf first, lowStage, highStage;

        void set (double sampleRate, float hz)
        {
            first.set (sampleRate, hz, 0.7071f);
            lowStage.set (sampleRate, hz, 0.7071f);
            highStage.set (sampleRate, hz, 0.7071f);
        }

        void process (float x, float& low, float& high) noexcept
        {
            float l, b, h, unused1, unused2;
            first.process (x, l, b, h);
            lowStage.process (l, low, unused1, unused2);
            highStage.process (h, unused1, unused2, high);
        }

        void reset()    { first.reset(); lowStage.reset(); highStage.reset(); }
    };
}

//==============================================================================
class DeEsserPlugin  : public CotoFxPlugin
{
public:
    explicit DeEsserPlugin (te::PluginCreationInfo);
    ~DeEsserPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("De-esser"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "De-esser"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "De-ess"; }
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

    te::AutomatableParameter *frequency, *threshold, *range, *mode, *listen;
    std::atomic<float> lastReduction { 0.0f };   // dB (<= 0)

private:
    double sampleRate = 44100.0;
    morefx::Svf filters[2];
    morefx::Follower follower;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeEsserPlugin)
};

//==============================================================================
class MultibandPlugin  : public CotoFxPlugin
{
public:
    explicit MultibandPlugin (te::PluginCreationInfo);
    ~MultibandPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Multiband compressor"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Multiband compressor"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Multiband"; }
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

    // [0] low, [1] mid, [2] high
    te::AutomatableParameter *threshold[3], *ratio[3], *gain[3];
    te::AutomatableParameter *lowCross, *highCross, *attack, *release;

private:
    double sampleRate = 44100.0;
    morefx::Crossover lowSplit[2], highSplit[2], lowAlign[2];   // per side
    morefx::Follower followers[3];
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandPlugin)
};

//==============================================================================
class DistortionPlugin  : public CotoFxPlugin
{
public:
    explicit DistortionPlugin (te::PluginCreationInfo);
    ~DistortionPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Distortion"); }
    static const char* xmlTypeName;
    static juce::StringArray typeNames();

    juce::String getName() const override               { return "Distortion"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Dist"; }
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

    te::AutomatableParameter *drive, *shape, *tone, *bits, *downsample, *mix, *output;

private:
    double sampleRate = 44100.0;
    float toneState[2] {}, held[2] {};
    int holdCount = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DistortionPlugin)
};

//==============================================================================
class StereoShaperPlugin  : public CotoFxPlugin
{
public:
    explicit StereoShaperPlugin (te::PluginCreationInfo);
    ~StereoShaperPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Stereo shaper"); }
    static const char* xmlTypeName;
    static juce::StringArray channelNames();

    juce::String getName() const override               { return "Stereo shaper"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "Stereo"; }
    juce::String getSelectableDescription() override    { return getName(); }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }
    double getTailLength() const override               { return 0.05; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    te::AutomatableParameter *width, *delay, *bassMono, *pan, *output, *channels;

private:
    double sampleRate = 44100.0;
    std::vector<float> line;   // the delayed side
    int writePos = 0;
    morefx::Svf bassFilter;    // takes the bass out of the sides
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoShaperPlugin)
};

//==============================================================================
class LoudnessMeterPlugin  : public CotoFxPlugin
{
public:
    explicit LoudnessMeterPlugin (te::PluginCreationInfo);
    ~LoudnessMeterPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Loudness meter"); }
    static const char* xmlTypeName;
    static constexpr float silence = -100.0f;   // "nothing measured yet"

    juce::String getName() const override               { return "Loudness meter"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "LUFS"; }
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

    /** Starts the integrated loudness and the peak again (from the window). */
    void restartMeasuring()                             { restart = true; }

    // What the window shows: LUFS (momentary = last 0.4 s, short-term = last 3 s,
    // integrated = everything since the last restart, quiet gaps left out) and dB peak.
    std::atomic<float> momentary { silence }, shortTerm { silence }, integrated { silence }, peak { silence };

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static constexpr int numSlots = 30;       // 3 s of 0.1 s pieces
    static constexpr int numBins = 750;       // -70 .. +5 LUFS in 0.1 steps

    double sampleRate = 44100.0;
    Biquad shelf[2], highPass[2];
    double slots[numSlots] {};
    int slotIndex = 0, slotsFilled = 0, slotSamples = 0, slotLength = 4410;
    double slotSum = 0.0;
    float peakGain = 0.0f;
    std::vector<int> histogram;
    std::atomic<bool> restart { false };

    void clearMeasurement();
    void finishSlot();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterPlugin)
};

//==============================================================================
/** The Loudness meter's window: four big numbers, a bar for "now" and Restart. */
class LoudnessMeterEditor  : public juce::Component,
                             private juce::Timer
{
public:
    explicit LoudnessMeterEditor (LoudnessMeterPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    LoudnessMeterPlugin& plugin;
    juce::TextButton restartButton;

    void timerCallback() override   { repaint(); }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterEditor)
};

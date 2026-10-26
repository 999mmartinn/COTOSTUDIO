#pragma once

#include <JuceHeader.h>
#include "ParametricEQ.h"

namespace te = tracktion;

//==============================================================================
/**
    COTOSTUDIO's 7-band parametric EQ (a built-in mixer effect).

    Every band has a type, a slope, a frequency, a gain and a width (Q); there is a
    main level too. All of them are automatable parameters stored in the song, so they
    sync to the other PCs and can be automated like any knob. Band solo (Shift in the
    editor) and the spectrum are personal: they only affect this PC.
*/
class ParametricEQPlugin  : public te::Plugin
{
public:
    explicit ParametricEQPlugin (te::PluginCreationInfo);
    ~ParametricEQPlugin() override;

    static const char* getPluginName()                  { return NEEDS_TRANS ("Parametric EQ"); }
    static const char* xmlTypeName;

    juce::String getName() const override               { return "Parametric EQ"; }
    juce::String getPluginType() override               { return xmlTypeName; }
    juce::String getShortName (int) override            { return "EQ"; }
    juce::String getSelectableDescription() override    { return getName(); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    BusLayout getBusses() const override                { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override                      { return false; }
    bool takesAudioInput() override                     { return true; }
    bool producesAudioWhenNoAudioInput() override       { return false; }
    double getTailLength() const override               { return 0.1; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override                        {}
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    //==============================================================================
    struct BandParams
    {
        te::AutomatableParameter::Ptr type, order, freq, gain, q;
        juce::CachedValue<float> typeValue, orderValue, freqValue, gainValue, qValue;
    };

    std::array<BandParams, peq::numBands> bands;
    te::AutomatableParameter::Ptr mainParam;
    juce::CachedValue<float> mainValue;

    /** The settings as the knobs say right now (any thread). */
    std::array<peq::Band, peq::numBands> getBands() const;
    double getMainDb() const                            { return mainParam->getCurrentValue(); }
    static peq::Band defaultBand (int index);

    /** Sets one band (message thread; one undo step should be started by the caller). */
    void setBand (int index, const peq::Band&);

    /** Personal (this PC only): hear just one band's range, -1 = off. */
    void setSoloBand (int band)                         { processor.setSoloBand (band); soloBand = band; }
    int getSoloBand() const                             { return soloBand; }

    //==============================================================================
    /** The spectrum feed for the editor: only filled while an editor is open. */
    struct Analyser
    {
        static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;   // 4096 points
        std::atomic<bool> wanted { false };
        juce::AbstractFifo fifo { fftSize * 4 };
        std::vector<float> buffer = std::vector<float> ((size_t) fftSize * 4);

        void push (const float* left, const float* right, int n)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (n, s1, n1, s2, n2);

            for (int i = 0; i < n1; ++i) buffer[(size_t) (s1 + i)] = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
            for (int i = 0; i < n2; ++i) buffer[(size_t) (s2 + i)] = right != nullptr ? 0.5f * (left[n1 + i] + right[n1 + i]) : left[n1 + i];

            fifo.finishedWrite (n1 + n2);
        }
    };

    Analyser analyser;
    double getSampleRate() const                        { return sampleRate; }

private:
    peq::Processor processor;
    double sampleRate = 44100.0;
    int soloBand = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEQPlugin)
};

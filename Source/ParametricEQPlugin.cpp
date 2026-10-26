#include "ParametricEQPlugin.h"

const char* ParametricEQPlugin::xmlTypeName = "cotoeq";

static juce::NormalisableRange<float> freqRange()
{
    juce::NormalisableRange<float> r (10.0f, 22000.0f);
    r.setSkewForCentre (630.0f);   // about half way up the audible range on the knob
    return r;
}

static juce::NormalisableRange<float> qRange()
{
    juce::NormalisableRange<float> r (0.1f, 24.0f);
    r.setSkewForCentre (1.0f);
    return r;
}

static juce::String hzText (float hz)
{
    return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz"
                         : juce::String (juce::roundToInt (hz)) + " Hz";
}

peq::Band ParametricEQPlugin::defaultBand (int index)
{
    // A shelf at each end and five peaks spread over the range, all flat: no effect
    // (and no CPU) until you move one.
    static const double freqs[] = { 60.0, 150.0, 400.0, 1000.0, 2500.0, 6000.0, 12000.0 };
    peq::Band b;
    b.type = index == 0 ? peq::lowShelf : index == peq::numBands - 1 ? peq::highShelf : peq::peak;
    b.order = 0;
    b.freq = freqs[juce::jlimit (0, peq::numBands - 1, index)];
    b.gain = 0.0;
    b.q = b.type == peq::peak ? 1.0 : 0.707;
    return b;
}

ParametricEQPlugin::ParametricEQPlugin (te::PluginCreationInfo info) : Plugin (info)
{
    auto um = getUndoManager();

    for (int i = 0; i < peq::numBands; ++i)
    {
        auto& b = bands[(size_t) i];
        const auto d = defaultBand (i);
        const juce::String n (i + 1);

        b.typeValue.referTo  (state, "type"  + n, um, (float) d.type);
        b.orderValue.referTo (state, "order" + n, um, (float) d.order);
        b.freqValue.referTo  (state, "freq"  + n, um, (float) d.freq);
        b.gainValue.referTo  (state, "gain"  + n, um, (float) d.gain);
        b.qValue.referTo     (state, "q"     + n, um, (float) d.q);

        b.type = addParam ("type" + n, TRANS ("Band") + " " + n + " " + TRANS ("type"), { 0.0f, (float) (peq::numTypes - 1), 1.0f },
                           [] (float v) { return TRANS (peq::typeName (juce::roundToInt (v))); }, {});
        b.order = addParam ("order" + n, TRANS ("Band") + " " + n + " " + TRANS ("slope"), { 0.0f, 3.0f, 1.0f },
                            [] (float v) { return juce::String (peq::dbPerOctave (juce::roundToInt (v))) + " dB/oct"; }, {});
        b.freq = addParam ("freq" + n, TRANS ("Band") + " " + n + " " + TRANS ("frequency"), freqRange(),
                           [] (float v) { return hzText (v); }, {});
        b.gain = addParam ("gain" + n, TRANS ("Band") + " " + n + " " + TRANS ("gain"), { -18.0f, 18.0f },
                           [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }, {});
        b.q = addParam ("q" + n, TRANS ("Band") + " " + n + " " + TRANS ("width (Q)"), qRange(),
                        [] (float v) { return "Q " + juce::String (v, 2); }, {});

        b.type->attachToCurrentValue (b.typeValue);
        b.order->attachToCurrentValue (b.orderValue);
        b.freq->attachToCurrentValue (b.freqValue);
        b.gain->attachToCurrentValue (b.gainValue);
        b.q->attachToCurrentValue (b.qValue);
    }

    mainValue.referTo (state, "main", um, 0.0f);
    mainParam = addParam ("main", TRANS ("Main level"), { -24.0f, 12.0f },
                          [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }, {});
    mainParam->attachToCurrentValue (mainValue);
}

ParametricEQPlugin::~ParametricEQPlugin()
{
    notifyListenersOfDeletion();

    for (auto p : getAutomatableParameters())
        p->detachFromCurrentValue();
}

void ParametricEQPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate > 0 ? info.sampleRate : 44100.0;
    processor.prepare (sampleRate);
    processor.setTarget (getBands(), getMainDb());
    processor.reset();
}

void ParametricEQPlugin::reset()
{
    processor.reset();
}

void ParametricEQPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    for (auto& b : bands)
        te::copyPropertiesToCachedValues (v, b.typeValue, b.orderValue, b.freqValue, b.gainValue, b.qValue);

    te::copyPropertiesToCachedValues (v, mainValue);

    for (auto p : getAutomatableParameters())
        p->updateFromAttachedValue();
}

std::array<peq::Band, peq::numBands> ParametricEQPlugin::getBands() const
{
    std::array<peq::Band, peq::numBands> result;

    for (int i = 0; i < peq::numBands; ++i)
    {
        auto& p = bands[(size_t) i];
        auto& b = result[(size_t) i];
        b.type  = juce::jlimit (0, peq::numTypes - 1, juce::roundToInt (p.type->getCurrentValue()));
        b.order = juce::jlimit (0, 3, juce::roundToInt (p.order->getCurrentValue()));
        b.freq  = p.freq->getCurrentValue();
        b.gain  = p.gain->getCurrentValue();
        b.q     = p.q->getCurrentValue();
    }

    return result;
}

void ParametricEQPlugin::setBand (int index, const peq::Band& b)
{
    if (! juce::isPositiveAndBelow (index, peq::numBands))
        return;

    auto& p = bands[(size_t) index];
    p.type->setParameter ((float) b.type, juce::sendNotification);
    p.order->setParameter ((float) b.order, juce::sendNotification);
    p.freq->setParameter ((float) b.freq, juce::sendNotification);
    p.gain->setParameter ((float) b.gain, juce::sendNotification);
    p.q->setParameter ((float) b.q, juce::sendNotification);
}

void ParametricEQPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int start = fc.bufferStartSample, num = fc.bufferNumSamples;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());

    float* channels[2] = { buffer.getWritePointer (0, start),
                           numChannels > 1 ? buffer.getWritePointer (1, start) : nullptr };

    processor.setTarget (getBands(), getMainDb());
    processor.process (channels, numChannels, num);

    if (analyser.wanted.load())
        analyser.push (channels[0], channels[1], num);
}

#include "SidechainPlugin.h"

const char* SidechainPlugin::xmlTypeName = "cotosidechain";

SidechainPlugin::SidechainPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    amount      = makeParam ("amount",      TRANS ("Amount"),      { 0.0f, 1.0f }, 0.7f, pctText);
    sensitivity = makeParam ("sensitivity", TRANS ("Sensitivity"), { -60.0f, 0.0f }, -30.0f, dbText);
    attack      = makeParam ("attack",      TRANS ("Attack"),      skewed (0.1f, 50.0f, 5.0f), 1.0f, msText);
    release     = makeParam ("release",     TRANS ("Release"),     skewed (10.0f, 1000.0f, 150.0f), 250.0f, msText);
}

SidechainPlugin::~SidechainPlugin()
{
    notifyListenersOfDeletion();
}

void SidechainPlugin::getChannelNames (juce::StringArray* ins, juce::StringArray* outs)
{
    // Inputs 3 and 4 are the sound it listens to (so the engine offers a sidechain).
    if (ins != nullptr)
        ins->addArray ({ TRANS ("Left"), TRANS ("Right"), TRANS ("Trigger left"), TRANS ("Trigger right") });

    if (outs != nullptr)
        outs->addArray ({ TRANS ("Left"), TRANS ("Right") });
}

void SidechainPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
    reset();
}

void SidechainPlugin::reset()
{
    fast = slow = 0.0f;
    armed = true;
    sinceHit = 1 << 30;
    gain = dipFrom = 1.0f;
}

void SidechainPlugin::applyPreset (int which)
{
    struct Preset { float amount, sensitivity, attack, release; };
    static constexpr Preset presets[] = { { 0.4f, -30.0f, 5.0f, 200.0f },    // gentle: a light dip
                                          { 0.8f, -30.0f, 2.0f, 350.0f },    // pumping: a long swell back (about a beat at 128 BPM)
                                          { 1.0f, -36.0f, 0.5f, 150.0f } };  // hard: gone at the hit, back fast

    const auto& p = presets[juce::jlimit (0, 2, which)];
    edit.getUndoManager().beginNewTransaction();
    amount->setParameter (p.amount, juce::sendNotification);
    sensitivity->setParameter (p.sensitivity, juce::sendNotification);
    attack->setParameter (p.attack, juce::sendNotification);
    release->setParameter (p.release, juce::sendNotification);
}

void SidechainPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int start = fc.bufferStartSample, n = fc.bufferNumSamples;
    const int channels = buffer.getNumChannels();

    // No trigger connected: the sound passes through as it is.
    if (channels < 3)
    {
        hearingTrigger = false;
        gain = 1.0f;
        sinceHit = 1 << 30;
        return;
    }

    const float depth = juce::jlimit (0.0f, 1.0f, amount->getCurrentValue());
    const float threshold = juce::Decibels::decibelsToGain (sensitivity->getCurrentValue());
    const auto sr = (float) sampleRate;
    const int attackSamples = juce::jmax (1, (int) (attack->getCurrentValue() * 0.001f * sr));
    const int releaseSamples = juce::jmax (1, (int) (release->getCurrentValue() * 0.001f * sr));

    // Hit detection: the quick follower jumps with a hit, the slow one lags behind it; a hit
    // is the quick one above the sensitivity and twice (+6 dB) the slow one. Then it waits
    // for that hit to pass (and at least 40 ms) before taking the next one.
    auto coefficient = [sr] (float ms) { return std::exp (-1.0f / juce::jmax (1.0f, ms * 0.001f * sr)); };
    const float fastUp = coefficient (0.3f), fastDown = coefficient (30.0f);
    const float slowUp = coefficient (25.0f), slowDown = coefficient (60.0f);
    const int refractory = (int) (0.04f * sr);
    const float floorGain = 1.0f - depth;

    const float* trigL = buffer.getReadPointer (2, start);
    const float* trigR = channels > 3 ? buffer.getReadPointer (3, start) : trigL;
    float* left = buffer.getWritePointer (0, start);
    float* right = buffer.getWritePointer (1, start);

    float peak = 0.0f, lowest = 1.0f;

    for (int i = 0; i < n; ++i)
    {
        const float t = juce::jmax (std::abs (trigL[i]), std::abs (trigR[i]));
        peak = juce::jmax (peak, t);
        fast = t > fast ? t + (fast - t) * fastUp : fast * fastDown;
        slow = t > slow ? t + (slow - t) * slowUp : slow * slowDown;
        JUCE_UNDENORMALISE (fast);
        JUCE_UNDENORMALISE (slow);

        if (sinceHit < (1 << 30))
            ++sinceHit;

        if (armed && fast > threshold && fast > 2.0f * slow)
        {
            armed = false;
            sinceHit = 0;
            dipFrom = gain;   // (a hit during a dip starts from where it is)
        }
        else if (! armed && sinceHit > refractory && fast < 1.5f * slow)
        {
            armed = true;
        }

        // The dip: down to the floor in the attack time, then a smooth way back up.
        if (sinceHit < attackSamples)
        {
            const float x = (float) sinceHit / (float) attackSamples;
            gain = dipFrom + (floorGain - dipFrom) * x;
        }
        else if (sinceHit < attackSamples + releaseSamples)
        {
            const float x = (float) (sinceHit - attackSamples) / (float) releaseSamples;
            gain = floorGain + depth * (x * x * (3.0f - 2.0f * x));
        }
        else
        {
            gain = 1.0f;
        }

        left[i] *= gain;
        right[i] *= gain;
        lowest = juce::jmin (lowest, gain);
    }

    // The trigger isn't part of what this track plays on.
    for (int c = 2; c < channels; ++c)
        buffer.clear (c, start, n);

    hearingTrigger = true;
    triggerPeak = juce::jmax (triggerPeak.load(), peak);
    lowestGain = juce::jmin (lowestGain.load(), lowest);
}

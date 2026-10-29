#include "LimiterPlugin.h"

const char* LimiterPlugin::xmlTypeName = "cotolimiter";

LimiterPlugin::LimiterPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    auto curveText = [] (float v) { return juce::String (juce::roundToInt (v)); };
    auto ratioText = [] (float v) { return v >= 29.5f ? juce::String ("inf : 1") : juce::String (v, v < 10.0f ? 1 : 0) + " : 1"; };
    auto threshText = [] (float v) { return v >= -0.05f ? TRANS ("off") : dbText (v); };
    auto ceilText = [] (float v) { return v >= 11.95f ? TRANS ("off") : dbText (v); };
    auto satText = [] (float v) { return v <= 0.05f ? TRANS ("off") : juce::String (v, 1) + " dB"; };
    auto gateText = [] (float v) { return v >= 0.995f ? TRANS ("off") : pctText (v); };

    compGain    = makeParam ("compGain",    TRANS ("Comp gain"),      { -24.0f, 24.0f }, 0.0f, dbText);
    compThresh  = makeParam ("compThresh",  TRANS ("Comp threshold"), { -60.0f, 0.0f }, 0.0f, threshText);
    compRatio   = makeParam ("compRatio",   TRANS ("Comp ratio"),     skewed (1.0f, 30.0f, 4.0f), 4.0f, ratioText);
    compKnee    = makeParam ("compKnee",    TRANS ("Comp knee"),      { 0.0f, 24.0f }, 0.0f, dbText);
    compAttack  = makeParam ("compAttack",  TRANS ("Comp attack"),    skewed (0.0f, 300.0f, 20.0f), 2.0f, msText);
    compRelease = makeParam ("compRelease", TRANS ("Comp release"),   skewed (1.0f, 2000.0f, 150.0f), 100.0f, msText);
    compCurve   = makeParam ("compCurve",   TRANS ("Comp curve"),     { 1.0f, 8.0f, 1.0f }, 2.0f, curveText);
    compSustain = makeParam ("compSustain", TRANS ("Comp sustain (RMS)"), skewed (0.0f, 1000.0f, 100.0f), 0.0f, msText);

    limGain     = makeParam ("limGain",     TRANS ("Limiter gain"),   { -24.0f, 24.0f }, 0.0f, dbText);
    ceiling     = makeParam ("ceiling",     TRANS ("Ceiling"),        { -30.0f, 12.0f }, 0.0f, ceilText);
    limAttack   = makeParam ("limAttack",   TRANS ("Limiter attack (look-ahead)"), { 0.0f, (float) lim::Processor::maxLookaheadMs }, 1.0f, msText);
    limRelease  = makeParam ("limRelease",  TRANS ("Limiter release"), skewed (1.0f, 2000.0f, 150.0f), 80.0f, msText);
    limCurve    = makeParam ("limCurve",    TRANS ("Limiter curve"),  { 1.0f, 8.0f, 1.0f }, 2.0f, curveText);
    limSustain  = makeParam ("limSustain",  TRANS ("Limiter sustain (hold)"), skewed (0.0f, 1000.0f, 100.0f), 0.0f, msText);

    gateGain    = makeParam ("gateGain",    TRANS ("Gate gain"),      { 0.0f, 1.0f }, 1.0f, gateText);
    gateThresh  = makeParam ("gateThresh",  TRANS ("Gate threshold"), { -80.0f, 0.0f }, -60.0f, dbText);
    gateRelease = makeParam ("gateRelease", TRANS ("Gate release"),   skewed (1.0f, 2000.0f, 150.0f), 100.0f, msText);
    saturation  = makeParam ("saturation",  TRANS ("Saturation"),     { 0.0f, 24.0f }, 0.0f, satText);

    processor.onFrame = [this] (const lim::Frame& f)
    {
        if (! displayWanted.load())
            return;

        int s1, n1, s2, n2;
        frameFifo.prepareToWrite (1, s1, n1, s2, n2);

        if (n1 > 0)
        {
            frames[(size_t) s1] = f;
            frameFifo.finishedWrite (1);
        }
    };

    latencySamples = lookaheadFromKnob();
    startTimer (250);
}

LimiterPlugin::~LimiterPlugin()
{
    stopTimer();
    notifyListenersOfDeletion();
}

int LimiterPlugin::lookaheadFromKnob() const
{
    return juce::roundToInt (limAttack->getCurrentValue() * 0.001 * sampleRate);
}

void LimiterPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate > 0 ? info.sampleRate : 44100.0;
    processor.prepare (sampleRate, info.blockSizeSamples);
    latencySamples = lookaheadFromKnob();
    processor.setLookaheadSamples (latencySamples.load());
    pendingLatency = -1;
}

void LimiterPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    CotoFxPlugin::restorePluginStateFromValueTree (v);
}

void LimiterPlugin::timerCallback()
{
    // The attack knob changed the look-ahead: once it has stayed put for a moment, tell
    // the engine (rebuild playback) so the delay compensation matches.
    const int wanted = lookaheadFromKnob();

    if (wanted == latencySamples.load())
    {
        pendingLatency = -1;
        return;
    }

    if (wanted != pendingLatency)
    {
        pendingLatency = wanted;   // still moving: wait for the next tick
        return;
    }

    latencySamples = wanted;
    pendingLatency = -1;
    edit.restartPlayback();
}

lim::Settings LimiterPlugin::getSettings() const
{
    lim::Settings s;
    s.compGainDb    = compGain->getCurrentValue();
    s.compThreshDb  = compThresh->getCurrentValue();
    s.compRatio     = compRatio->getCurrentValue() >= 29.5f ? 1000.0 : compRatio->getCurrentValue();
    s.compKneeDb    = compKnee->getCurrentValue();
    s.compAttackMs  = compAttack->getCurrentValue();
    s.compReleaseMs = compRelease->getCurrentValue();
    s.compCurve     = juce::roundToInt (compCurve->getCurrentValue());
    s.compSustainMs = compSustain->getCurrentValue();
    s.limGainDb     = limGain->getCurrentValue();
    s.ceilingDb     = ceiling->getCurrentValue();
    s.limReleaseMs  = limRelease->getCurrentValue();
    s.limCurve      = juce::roundToInt (limCurve->getCurrentValue());
    s.limSustainMs  = limSustain->getCurrentValue();
    s.gateGain      = gateGain->getCurrentValue();
    s.gateThreshDb  = gateThresh->getCurrentValue();
    s.gateReleaseMs = gateRelease->getCurrentValue();
    s.satDb         = saturation->getCurrentValue();
    return s;
}

void LimiterPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    float* channels[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                           numChannels > 1 ? buffer.getWritePointer (1, fc.bufferStartSample) : nullptr };

    processor.setSettings (getSettings());
    processor.setLookaheadSamples (latencySamples.load());
    processor.process (channels, numChannels, fc.bufferNumSamples);
}

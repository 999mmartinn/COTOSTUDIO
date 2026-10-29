#include "ReverbPlugin.h"

const char* ReverbPlugin::xmlTypeName = "cotoreverb";

static constexpr float dampOff = 20000.0f;   // the knob's top = no damping

ReverbPlugin::ReverbPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    auto secondsText = [] (float v) { return juce::String (v, v < 10.0f ? 2 : 1) + " s"; };
    auto timesText = [] (float v) { return juce::String (v, 2) + "x"; };
    auto divisionText = [] (float v) { return juce::String (dly::divisions[juce::jlimit (0, dly::numDivisions - 1, juce::roundToInt (v))].name); };

    lowCut     = makeParam ("lowCut",     TRANS ("Low cut"),          skewed (10.0f, 2000.0f, 150.0f), 70.0f, hzText);
    highCut    = makeParam ("highCut",    TRANS ("High cut"),         skewed (1000.0f, 20000.0f, 6000.0f), 12000.0f,
                            [] (float v) { return v >= 19999.0f ? TRANS ("off") : hzText (v); });
    tempo      = makeParam ("tempo",      TRANS ("Pre-delay in tempo"), { 0.0f, 1.0f, 1.0f }, 0.0f,
                            [] (float v) { return v >= 0.5f ? TRANS ("on") : TRANS ("off"); });
    predelay   = makeParam ("predelay",   TRANS ("Pre-delay"),        skewed (0.0f, 500.0f, 60.0f), 20.0f, msText);
    predelayDivision = makeParam ("predelayDivision", TRANS ("Pre-delay (tempo)"), { 0.0f, (float) (dly::numDivisions - 1), 1.0f }, 4.0f, divisionText);
    size       = makeParam ("size",       TRANS ("Room size"),        { 0.0f, 1.0f }, 0.6f, pctText);
    diffusion  = makeParam ("diffusion",  TRANS ("Diffusion"),        { 0.0f, 1.0f }, 0.7f, pctText);
    mod        = makeParam ("mod",        TRANS ("Modulation"),       { 0.0f, 1.0f }, 0.2f, pctText);
    modSpeed   = makeParam ("modSpeed",   TRANS ("Mod speed"),        skewed (0.05f, 5.0f, 0.8f), 0.6f,
                            [] (float v) { return juce::String (v, 2) + " Hz"; });
    decay      = makeParam ("decay",      TRANS ("Decay"),            skewed (0.1f, 20.0f, 2.5f), 2.5f, secondsText);
    bass       = makeParam ("bass",       TRANS ("Bass multiplier"),  skewed (0.25f, 4.0f, 1.0f), 1.0f, timesText);
    crossover  = makeParam ("crossover",  TRANS ("Bass crossover"),   skewed (50.0f, 2000.0f, 300.0f), 300.0f, hzText);
    damp       = makeParam ("damp",       TRANS ("High damping"),     skewed (500.0f, dampOff, 5000.0f), 8000.0f,
                            [] (float v) { return v >= dampOff - 1.0f ? TRANS ("off") : hzText (v); });
    dry        = makeParam ("dry",        TRANS ("Dry"),              { 0.0f, 1.0f }, 1.0f, pctText);
    early      = makeParam ("early",      TRANS ("Early reflections"), { 0.0f, 1.0f }, 0.4f, pctText);
    wet        = makeParam ("wet",        TRANS ("Wet"),              { 0.0f, 1.0f }, 0.25f, pctText);
    separation = makeParam ("separation", TRANS ("Stereo separation"), { 0.0f, 1.0f }, 1.0f, pctText);
    side       = makeParam ("side",       TRANS ("Mid / side"),       { 0.0f, 1.0f, 1.0f }, 0.0f,
                            [] (float v) { return v >= 0.5f ? TRANS ("Side") : TRANS ("Mid"); });

    readTempo();
    startTimerHz (10);
}

ReverbPlugin::~ReverbPlugin()
{
    stopTimer();
    notifyListenersOfDeletion();
}

void ReverbPlugin::readTempo()
{
    const double b = edit.tempoSequence.getBpmAt (edit.getTransport().getPosition());

    if (b > 1.0)
        bpm = b;
}

void ReverbPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    readTempo();
    processor.prepare (info.sampleRate > 0 ? info.sampleRate : 44100.0);
}

rvb::Settings ReverbPlugin::getSettings() const
{
    rvb::Settings s;
    s.lowCutHz         = lowCut->getCurrentValue();
    s.highCutHz        = highCut->getCurrentValue() >= 19999.0f ? 20000.0 : (double) highCut->getCurrentValue();
    s.tempoPredelay    = tempo->getCurrentValue() >= 0.5f;
    s.predelayMs       = predelay->getCurrentValue();
    s.predelayDivision = juce::roundToInt (predelayDivision->getCurrentValue());
    s.bpm              = bpm.load();
    s.size             = size->getCurrentValue();
    s.diffusion        = diffusion->getCurrentValue();
    s.modDepth         = mod->getCurrentValue();
    s.modSpeedHz       = modSpeed->getCurrentValue();
    s.decaySeconds     = decay->getCurrentValue();
    s.bassMultiplier   = bass->getCurrentValue();
    s.crossoverHz      = crossover->getCurrentValue();
    s.dampHz           = damp->getCurrentValue() >= dampOff - 1.0f ? 20000.0 : (double) damp->getCurrentValue();
    s.dry              = dry->getCurrentValue();
    s.early            = early->getCurrentValue();
    s.wet              = wet->getCurrentValue();
    s.separation       = separation->getCurrentValue();
    s.side             = side->getCurrentValue() >= 0.5f;
    return s;
}

void ReverbPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    float* channels[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                           numChannels > 1 ? buffer.getWritePointer (1, fc.bufferStartSample) : nullptr };

    processor.setSettings (getSettings());
    processor.process (channels, numChannels, fc.bufferNumSamples);
}

#include "DelayPlugin.h"

const char* DelayPlugin::xmlTypeName = "cotodelay";

static constexpr float maxSampleRateParam = 48000.0f;   // the knob's top = off

DelayPlugin::DelayPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    auto onOff = [] (float v) { return v >= 0.5f ? TRANS ("on") : TRANS ("off"); };
    auto modelText = [] (float v)
    {
        switch (juce::roundToInt (v)) { case 0: return TRANS ("Mono"); case 1: return TRANS ("Stereo"); case 2: return TRANS ("Ping pong"); default: return TRANS ("Off"); }
    };
    auto filterText = [] (float v)
    {
        switch (juce::roundToInt (v)) { case 1: return juce::String ("LP"); case 2: return juce::String ("HP"); case 3: return juce::String ("BP"); default: return TRANS ("off"); }
    };
    auto divisionText = [] (float v) { return juce::String (dly::divisions[juce::jlimit (0, dly::numDivisions - 1, juce::roundToInt (v))].name); };
    auto signedPct = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto toneText = [] (float v)
    {
        return std::abs (v) < 0.005f ? TRANS ("off") : (v < 0.0f ? "LP " : "HP ") + juce::String (juce::roundToInt (std::abs (v) * 100.0f)) + "%";
    };

    model      = makeParam ("model",      TRANS ("Model"),           { 0.0f, 3.0f, 1.0f }, (float) dly::stereo, modelText);
    input      = makeParam ("input",      TRANS ("Input"),           { 0.0f, 1.0f }, 1.0f, pctText);
    sync       = makeParam ("sync",       TRANS ("Tempo sync"),      { 0.0f, 1.0f, 1.0f }, 1.0f, onOff);
    division   = makeParam ("division",   TRANS ("Time (tempo)"),    { 0.0f, (float) (dly::numDivisions - 1), 1.0f }, 9.0f, divisionText);
    timeMs     = makeParam ("time",       TRANS ("Time"),            skewed (1.0f, 1000.0f, 200.0f), 300.0f, msText);
    keepPitch  = makeParam ("keepPitch",  TRANS ("Keep pitch"),      { 0.0f, 1.0f, 1.0f }, 1.0f, onOff);
    smoothing  = makeParam ("smoothing",  TRANS ("Smoothing"),       { 0.0f, 1.0f }, 0.0f, pctText);
    offset     = makeParam ("offset",     TRANS ("Offset / pan"),    { -1.0f, 1.0f }, 0.0f, signedPct);
    spread     = makeParam ("spread",     TRANS ("Stereo spread"),   { 0.0f, 1.0f }, 1.0f, pctText);

    feedback   = makeParam ("feedback",   TRANS ("Feedback"),        { 0.0f, 1.2f }, 0.4f, pctText);
    filterType = makeParam ("filterType", TRANS ("Filter type"),     { 0.0f, 3.0f, 1.0f }, (float) dly::lowPass, filterText);
    cutoff     = makeParam ("cutoff",     TRANS ("Cutoff"),          skewed (20.0f, 20000.0f, 1000.0f), 6000.0f, hzText);
    resonance  = makeParam ("resonance",  TRANS ("Resonance"),       { 0.0f, 1.0f }, 0.0f, pctText);
    sampleRate = makeParam ("sampleRate", TRANS ("Sample rate"),     skewed (500.0f, maxSampleRateParam, 8000.0f), maxSampleRateParam,
                            [] (float v) { return v >= maxSampleRateParam - 1.0f ? TRANS ("off") : hzText (v); });
    bits       = makeParam ("bits",       TRANS ("Bits"),            { 1.0f, 24.0f, 1.0f }, 24.0f,
                            [] (float v) { return v >= 23.5f ? TRANS ("off") : juce::String (juce::roundToInt (v)) + " bit"; });

    modRate    = makeParam ("modRate",    TRANS ("Mod rate"),        skewed (0.0f, 20.0f, 2.0f), 0.5f,
                            [] (float v) { return juce::String (v, v < 10.0f ? 2 : 1) + " Hz"; });
    modTime    = makeParam ("modTime",    TRANS ("Mod time"),        { 0.0f, 1.0f }, 0.0f, pctText);
    modCutoff  = makeParam ("modCutoff",  TRANS ("Mod cutoff"),      { 0.0f, 1.0f }, 0.0f, pctText);
    diffusion  = makeParam ("diffusion",  TRANS ("Diffusion"),       { 0.0f, 1.0f }, 0.0f, pctText);
    diffusionSpread = makeParam ("diffusionSpread", TRANS ("Diffusion spread"), { 0.0f, 1.0f }, 0.5f, pctText);

    distType   = makeParam ("distType",   TRANS ("Distortion type"), { 0.0f, 1.0f, 1.0f }, (float) dly::limit,
                            [] (float v) { return v >= 0.5f ? TRANS ("Sat") : TRANS ("Limit"); });
    distKnee   = makeParam ("distKnee",   TRANS ("Knee"),            { 0.0f, 1.0f }, 0.5f, pctText);
    distSymmetry = makeParam ("distSymmetry", TRANS ("Symmetry"),    { 0.0f, 1.0f }, 0.0f, pctText);
    distLevel  = makeParam ("distLevel",  TRANS ("Distortion level"), { -30.0f, 0.0f }, 0.0f, dbText);

    wet        = makeParam ("wet",        TRANS ("Wet"),             { 0.0f, 1.0f }, 0.5f, pctText);
    tone       = makeParam ("tone",       TRANS ("Tone"),            { -1.0f, 1.0f }, 0.0f, toneText);
    dry        = makeParam ("dry",        TRANS ("Dry"),             { 0.0f, 1.0f }, 1.0f, pctText);

    readTempo();
    startTimerHz (10);
}

DelayPlugin::~DelayPlugin()
{
    stopTimer();
    notifyListenersOfDeletion();
}

void DelayPlugin::readTempo()
{
    const double b = edit.tempoSequence.getBpmAt (edit.getTransport().getPosition());

    if (b > 1.0)
        bpm = b;
}

void DelayPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    rate = info.sampleRate > 0 ? info.sampleRate : 44100.0;
    readTempo();
    processor.prepare (rate);
}

dly::Settings DelayPlugin::getSettings() const
{
    dly::Settings s;
    s.model        = juce::roundToInt (model->getCurrentValue());
    s.inputLevel   = input->getCurrentValue();
    s.tempoSync    = sync->getCurrentValue() >= 0.5f;
    s.division     = juce::roundToInt (division->getCurrentValue());
    s.timeMs       = timeMs->getCurrentValue();
    s.bpm          = bpm.load();
    s.keepPitch    = keepPitch->getCurrentValue() >= 0.5f;
    s.smoothing    = smoothing->getCurrentValue();
    s.offset       = offset->getCurrentValue();
    s.spread       = spread->getCurrentValue();
    s.feedback     = feedback->getCurrentValue();
    s.filterType   = juce::roundToInt (filterType->getCurrentValue());
    s.cutoffHz     = cutoff->getCurrentValue();
    s.resonance    = resonance->getCurrentValue();
    s.sampleRateHz = sampleRate->getCurrentValue() >= maxSampleRateParam - 1.0f ? 1.0e9 : (double) sampleRate->getCurrentValue();
    s.bits         = bits->getCurrentValue() >= 23.5f ? 24.0 : (double) juce::roundToInt (bits->getCurrentValue());
    s.modRateHz    = modRate->getCurrentValue();
    s.modTime      = modTime->getCurrentValue();
    s.modCutoff    = modCutoff->getCurrentValue();
    s.diffusion    = diffusion->getCurrentValue();
    s.diffusionSpread = diffusionSpread->getCurrentValue();
    s.distType     = distType->getCurrentValue() >= 0.5f ? dly::saturate : dly::limit;
    s.distKnee     = distKnee->getCurrentValue();
    s.distSymmetry = distSymmetry->getCurrentValue();
    s.distLevelDb  = distLevel->getCurrentValue();
    s.wet          = wet->getCurrentValue();
    s.tone         = tone->getCurrentValue();
    s.dry          = dry->getCurrentValue();
    return s;
}

void DelayPlugin::applyToBuffer (const te::PluginRenderContext& fc)
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

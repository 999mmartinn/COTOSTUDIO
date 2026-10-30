#include "MoreFx.h"

const char* CompressorFxPlugin::xmlTypeName = "cotocomp";
const char* EnhancerPlugin::xmlTypeName = "cotoenhancer";
const char* FlangerPlugin::xmlTypeName = "cotoflanger";
const char* FilterFxPlugin::xmlTypeName = "cotofilter";

namespace
{
    juce::String ratioText (float v)     { return v >= 29.5f ? juce::String ("inf : 1") : juce::String (v, v < 10.0f ? 1 : 0) + " : 1"; }
    juce::String signedPct (float v)     { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v * 100.0f)) + "%"; }
    juce::String panText (float v)
    {
        const int p = juce::roundToInt (v * 100.0f);
        return p == 0 ? TRANS ("centre") : juce::String (std::abs (p)) + (p < 0 ? " L" : " R");
    }
}

//==============================================================================
juce::StringArray CompressorFxPlugin::typeNames()
{
    return { TRANS ("Hard"), TRANS ("Medium hard"), TRANS ("Medium"), TRANS ("Soft"),
             TRANS ("Hard RMS"), TRANS ("Medium hard RMS"), TRANS ("Medium RMS"), TRANS ("Soft RMS") };
}

CompressorFxPlugin::CompressorFxPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    threshold = makeParam ("threshold", TRANS ("Threshold"), { -60.0f, 0.0f }, -18.0f, dbText);
    ratio     = makeParam ("ratio",     TRANS ("Ratio"),     skewed (1.0f, 30.0f, 4.0f), 4.0f, ratioText);
    gain      = makeParam ("gain",      TRANS ("Gain"),      { -30.0f, 30.0f }, 0.0f, dbText);
    attack    = makeParam ("attack",    TRANS ("Attack"),    skewed (0.0f, 400.0f, 20.0f), 10.0f, msText);
    release   = makeParam ("release",   TRANS ("Release"),   skewed (1.0f, 4000.0f, 200.0f), 150.0f, msText);
    // ("compType": a plugin's "type" property is its kind, set by the engine)
    type      = makeParam ("compType",  TRANS ("Type"),      { 0.0f, 7.0f, 1.0f }, 2.0f,
                           [] (float v) { return typeNames()[juce::jlimit (0, 7, juce::roundToInt (v))]; });
}

CompressorFxPlugin::~CompressorFxPlugin()    { notifyListenersOfDeletion(); }

void CompressorFxPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    const int t = juce::jlimit (0, 7, juce::roundToInt (type->getCurrentValue()));
    static constexpr float knees[] = { 0.0f, 3.0f, 6.0f, 12.0f };
    const float knee = knees[t % 4];
    const bool rms = t >= 4;
    const float th = threshold->getCurrentValue(), r = ratio->getCurrentValue();
    const float makeup = gain->getCurrentValue();
    follower.setTimes (sampleRate, attack->getCurrentValue(), release->getCurrentValue());

    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };
    float most = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float l = data[0][i], rr = channels > 1 ? data[1][i] : l;
        const float level = rms ? 0.5f * (l * l + rr * rr) : juce::jmax (std::abs (l), std::abs (rr));
        const float env = follower.process (level);
        const float envDb = rms ? 10.0f * std::log10 (juce::jmax (1.0e-12f, env)) : juce::Decibels::gainToDecibels (env, -120.0f);
        const float reduction = morefx::compressDb (envDb, th, r, knee);
        const float g = juce::Decibels::decibelsToGain (reduction + makeup);
        most = juce::jmin (most, reduction);

        data[0][i] = l * g;

        if (channels > 1)
            data[1][i] = rr * g;
    }

    lastReduction = most;
}

//==============================================================================
EnhancerPlugin::EnhancerPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    mode   = makeParam ("mode",   TRANS ("Mode"),   { 0.0f, 3.0f, 1.0f }, 0.0f,
                        [] (float v) { return juce::String::charToString ((juce::juce_wchar) ('A' + juce::jlimit (0, 3, juce::roundToInt (v)))); });
    amount = makeParam ("amount", TRANS ("Amount"), { 0.0f, 1.0f }, 0.6f, pctText);
}

EnhancerPlugin::~EnhancerPlugin()    { notifyListenersOfDeletion(); }

void EnhancerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
    reset();
}

void EnhancerPlugin::reset()
{
    for (int c = 0; c < 2; ++c)
        lowA[c] = lowB[c] = highA[c] = highB[c] = 0.0f;

    for (auto& b : bands)
        b.value = 0.0f;

    limiter.value = 0.0f;
    smoothedAmount = amount->getCurrentValue();
}

void EnhancerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };

    // Each flavour leans on different bands (low, mid, high): A even, B fuller low end,
    // C brighter and more present (vocals), D strongest overall.
    static constexpr float weights[4][3] = { { 1.0f, 1.0f, 1.0f }, { 1.35f, 0.9f, 0.8f }, { 0.8f, 1.05f, 1.35f }, { 1.35f, 1.25f, 1.35f } };
    const auto& w = weights[juce::jlimit (0, 3, juce::roundToInt (mode->getCurrentValue()))];

    auto onePole = [this] (float hz) { return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sampleRate); };
    const float lowCoef = onePole (200.0f), highCoef = onePole (3000.0f);

    for (auto& b : bands)
        b.setTimes (sampleRate, 10.0f, 150.0f);

    limiter.setTimes (sampleRate, 0.0f, 120.0f);
    const float target = amount->getCurrentValue();

    for (int i = 0; i < n; ++i)
    {
        smoothedAmount += (target - smoothedAmount) * 0.001f;
        float split[2][3];
        float level[3] = {};

        for (int c = 0; c < channels; ++c)
        {
            const float x = data[c][i];
            lowA[c] += (x - lowA[c]) * lowCoef;          lowB[c] += (lowA[c] - lowB[c]) * lowCoef;
            highA[c] += (x - highA[c]) * highCoef;       highB[c] += (highA[c] - highB[c]) * highCoef;
            split[c][0] = lowB[c];
            split[c][1] = highB[c] - lowB[c];
            split[c][2] = x - highB[c];

            for (int b = 0; b < 3; ++b)
                level[b] = juce::jmax (level[b], std::abs (split[c][b]));
        }

        // Upward compression: quiet parts of each band come up (up to +12 dB); loud ones are
        // only eased a little (2 dB at most), so it gets fuller and louder, never quieter.
        float bandGain[3];

        for (int b = 0; b < 3; ++b)
        {
            const float envDb = juce::Decibels::gainToDecibels (bands[b].process (level[b]), -80.0f);
            const float wanted = juce::jlimit (-2.0f, 12.0f, (-20.0f - envDb) * 0.5f);
            bandGain[b] = juce::Decibels::decibelsToGain (wanted * smoothedAmount * w[b] * (envDb > -70.0f ? 1.0f : 0.0f));
        }

        float out[2];
        float peak = 0.0f;

        for (int c = 0; c < channels; ++c)
        {
            out[c] = split[c][0] * bandGain[0] + split[c][1] * bandGain[1] + split[c][2] * bandGain[2];
            peak = juce::jmax (peak, std::abs (out[c]));
        }

        // A safety limiter at about -0.3 dB.
        const float env = limiter.process (peak);
        const float g = env > 0.966f ? 0.966f / env : 1.0f;

        for (int c = 0; c < channels; ++c)
            data[c][i] = out[c] * g;
    }
}

//==============================================================================
FlangerPlugin::FlangerPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    order  = makeParam ("order",  TRANS ("Order"),  { 1.0f, (float) maxOrder, 1.0f }, 4.0f, [] (float v) { return juce::String (juce::roundToInt (v)); });
    depth  = makeParam ("depth",  TRANS ("Depth"),  { 0.0f, 1.0f }, 0.5f, pctText);
    speed  = makeParam ("speed",  TRANS ("Speed"),  { 0.0f, 1.0f }, 0.75f,
                        [] (float v) { return juce::String (0.05f * std::pow (100.0f, v), 2) + " Hz"; });
    delay  = makeParam ("delay",  TRANS ("Delay"),  { 0.0f, 1.0f }, 0.0625f,
                        [] (float v) { return juce::String (0.5f + 15.0f * v, 1) + " ms"; });
    spread = makeParam ("spread", TRANS ("Spread"), { 0.0f, 1.0f }, 1.0f, pctText);
    cross  = makeParam ("cross",  TRANS ("Cross"),  { -1.0f, 1.0f }, -0.25f, signedPct);
    dry    = makeParam ("dry",    TRANS ("Dry"),    { -1.0f, 1.0f }, 0.75f, signedPct);
    wet    = makeParam ("wet",    TRANS ("Wet"),    { -1.0f, 1.0f }, 0.75f, signedPct);
}

FlangerPlugin::~FlangerPlugin()    { notifyListenersOfDeletion(); }

void FlangerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
    line.setSize (2, (int) (sampleRate * 0.05) + 4);
    reset();
}

void FlangerPlugin::reset()
{
    line.clear();
    writePos = 0;
    feedback[0] = feedback[1] = 0.0f;
}

void FlangerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0 || line.getNumSamples() == 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };

    const int voices = juce::jlimit (1, maxOrder, juce::roundToInt (order->getCurrentValue()));
    const double rate = 0.05 * std::pow (100.0, (double) speed->getCurrentValue());
    const float baseMs = 0.5f + 15.0f * delay->getCurrentValue();
    const float sweepMs = 5.0f * depth->getCurrentValue();
    const double stereoOffset = 0.5 * spread->getCurrentValue();
    const float fb = 0.9f * cross->getCurrentValue();
    const float dryGain = dry->getCurrentValue(), wetGain = wet->getCurrentValue() / std::sqrt ((float) voices);
    const int size = line.getNumSamples();
    const double phaseStep = rate / sampleRate;

    for (int i = 0; i < n; ++i)
    {
        const float in[2] = { data[0][i], channels > 1 ? data[1][i] : data[0][i] };

        // Cross feedback: each side gets some of the other side's last echo.
        for (int c = 0; c < 2; ++c)
            line.setSample (c, writePos, in[c] + feedback[1 - c] * fb);

        float out[2] = {};

        for (int c = 0; c < 2; ++c)
        {
            float sum = 0.0f;

            for (int v = 0; v < voices; ++v)
            {
                const double p = phase + (double) v / voices + (c == 1 ? stereoOffset : 0.0);
                const float lfo = 0.5f + 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * p);
                const float delaySamples = (baseMs + sweepMs * lfo) * 0.001f * (float) sampleRate;
                float readPos = (float) writePos - delaySamples;

                while (readPos < 0.0f)
                    readPos += (float) size;

                const int a = (int) readPos;
                const float frac = readPos - (float) a;
                const float s0 = line.getSample (c, a % size), s1 = line.getSample (c, (a + 1) % size);
                sum += s0 + (s1 - s0) * frac;
            }

            feedback[c] = sum / (float) voices;
            JUCE_UNDENORMALISE (feedback[c]);
            out[c] = in[c] * dryGain + sum * wetGain;
        }

        data[0][i] = out[0];

        if (channels > 1)
            data[1][i] = out[1];

        writePos = (writePos + 1) % size;
        phase += phaseStep;

        if (phase >= 1.0)
            phase -= 1.0;
    }
}

//==============================================================================
FilterFxPlugin::FilterFxPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    inVol     = makeParam ("inVol",     TRANS ("In"),        { -24.0f, 12.0f }, 0.0f, dbText);
    stages    = makeParam ("stages",    TRANS ("Stages"),    { 0.0f, 3.0f, 1.0f }, 1.0f,
                           [] (float v) { const int s = juce::roundToInt (v); return s == 0 ? TRANS ("Off") : juce::String (s) + "x"; });
    cutoff    = makeParam ("cutoff",    TRANS ("Cutoff"),    skewed (20.0f, 20000.0f, 1000.0f), 1000.0f, hzText);
    resonance = makeParam ("resonance", TRANS ("Resonance"), { 0.0f, 1.0f }, 0.2f, pctText);
    lowMix    = makeParam ("low",       TRANS ("Low"),       { 0.0f, 1.0f }, 1.0f, pctText);
    bandMix   = makeParam ("band",      TRANS ("Band"),      { 0.0f, 1.0f }, 0.0f, pctText);
    highMix   = makeParam ("high",      TRANS ("High"),      { 0.0f, 1.0f }, 0.0f, pctText);
    pan       = makeParam ("pan",       TRANS ("Pan"),       { -1.0f, 1.0f }, 0.0f, panText);
    outVol    = makeParam ("outVol",    TRANS ("Out"),       { -24.0f, 12.0f }, 0.0f, dbText);
}

FilterFxPlugin::~FilterFxPlugin()    { notifyListenersOfDeletion(); }

void FilterFxPlugin::reset()
{
    for (auto& side : filters)
        for (auto& f : side)
            f.reset();
}

void FilterFxPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    const int count = juce::jlimit (0, 3, juce::roundToInt (stages->getCurrentValue()));
    const float q = 0.5f + 9.5f * resonance->getCurrentValue() * resonance->getCurrentValue();
    const float l = lowMix->getCurrentValue(), b = bandMix->getCurrentValue(), h = highMix->getCurrentValue();
    const float inGain = juce::Decibels::decibelsToGain (inVol->getCurrentValue());
    const float outGain = juce::Decibels::decibelsToGain (outVol->getCurrentValue());
    const float p = pan->getCurrentValue();
    const float sideGain[2] = { outGain * juce::jmin (1.0f, 1.0f - p), outGain * juce::jmin (1.0f, 1.0f + p) };

    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < count; ++s)
            filters[c][s].set (sampleRate, cutoff->getCurrentValue(), q);

    for (int c = 0; c < channels; ++c)
    {
        auto* d = buffer.getWritePointer (c, fc.bufferStartSample);

        for (int i = 0; i < n; ++i)
        {
            float y = d[i] * inGain;

            for (int s = 0; s < count; ++s)
            {
                float low, band, high;
                filters[c][s].process (y, low, band, high);
                y = low * l + band * b + high * h;
            }

            d[i] = y * sideGain[channels > 1 ? c : 0];
        }
    }
}

//==============================================================================
MoreFxEditor::MoreFxEditor (CotoFxPlugin& p) : plugin (p)
{
    using namespace theme::colours;

    if (auto* comp = dynamic_cast<CompressorFxPlugin*> (&p))
    {
        addKnobs (TRANS ("Compressor"), orange, { { comp->threshold, TRANS ("Threshold") }, { comp->ratio, TRANS ("Ratio") }, { comp->gain, TRANS ("Gain") } });
        addKnobs (TRANS ("Timing"), teal, { { comp->attack, TRANS ("Attack") }, { comp->release, TRANS ("Release") } });
        addChoice (TRANS ("Type"), amber, *comp->type, CompressorFxPlugin::typeNames());
        help = TRANS ("Above the threshold the sound is turned down by the ratio; gain brings it back up. Soft types bend in gently, RMS types follow the loudness instead of the peaks.");
    }
    else if (auto* enh = dynamic_cast<EnhancerPlugin*> (&p))
    {
        addKnobs (TRANS ("Enhancer"), orange, { { enh->amount, TRANS ("Amount") } });
        addChoice (TRANS ("Flavour"), amber, *enh->mode, { "A", "B", "C", "D" });
        help = TRANS ("Turn it up for a fuller, louder sound. A: even. B: more low end. C: brighter, more present (vocals). D: strongest.");
    }
    else if (auto* fl = dynamic_cast<FlangerPlugin*> (&p))
    {
        addKnobs (TRANS ("Sweep"), teal, { { fl->order, TRANS ("Order") }, { fl->depth, TRANS ("Depth") }, { fl->speed, TRANS ("Speed") }, { fl->delay, TRANS ("Delay") } });
        addKnobs (TRANS ("Mix"), orange, { { fl->spread, TRANS ("Spread") }, { fl->cross, TRANS ("Cross") }, { fl->dry, TRANS ("Dry") }, { fl->wet, TRANS ("Wet") } },
                  { fl->cross, fl->dry, fl->wet });
        help = TRANS ("Order: how many swept copies. Spread widens the stereo. Cross feeds each side's echo into the other. Dry or wet below zero turns that part upside down (a hollower sound).");
    }
    else if (auto* f = dynamic_cast<FilterFxPlugin*> (&p))
    {
        addKnobs (TRANS ("Filter"), teal, { { f->inVol, TRANS ("In") }, { f->cutoff, TRANS ("Cutoff") }, { f->resonance, TRANS ("Resonance") } });
        addKnobs (TRANS ("Mix"), orange, { { f->lowMix, TRANS ("Low") }, { f->bandMix, TRANS ("Band") }, { f->highMix, TRANS ("High") }, { f->pan, TRANS ("Pan") }, { f->outVol, TRANS ("Out") } },
                  { f->pan });
        addChoice (TRANS ("Stages"), amber, *f->stages, { TRANS ("Off"), "1x", "2x", "3x" });
        help = TRANS ("Low, band and high are how much of each part of the filter you hear: low only = muffled, band only = telephone. More stages = steeper.");
    }

    int width = 0;

    for (auto& r : rows)
        width = juce::jmax (width, r.items.size() * 86 + 28);

    width = juce::jmax (width, choiceButtons * 62 + 48);   // room for every button's name

    width = juce::jmax (360, width);
    int height = 12;

    for (auto& r : rows)
        height += (dynamic_cast<fxui::ParamChoice*> (r.items.getFirst()) != nullptr ? 64 : 136) + 10;

    setSize (width, height + 52);
}

void MoreFxEditor::addKnobs (const juce::String& title, juce::Colour colour,
                             std::initializer_list<std::pair<te::AutomatableParameter*, juce::String>> knobs,
                             std::initializer_list<te::AutomatableParameter*> bipolar)
{
    Row row { title, colour, {}, {} };

    for (auto& [param, label] : knobs)
    {
        const bool bi = std::find (bipolar.begin(), bipolar.end(), param) != bipolar.end();
        auto* k = controls.add (new fxui::ParamKnob (*param, label, colour, bi));
        addAndMakeVisible (k);
        row.items.add (k);
    }

    rows.push_back (row);
}

void MoreFxEditor::addChoice (const juce::String& title, juce::Colour colour, te::AutomatableParameter& param, const juce::StringArray& names)
{
    choiceButtons = juce::jmax (choiceButtons, names.size());
    auto* c = controls.add (new fxui::ParamChoice (param, names, colour));
    addAndMakeVisible (c);
    rows.push_back ({ title, colour, { c }, {} });
}

void MoreFxEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    for (auto& row : rows)
    {
        const bool choice = dynamic_cast<fxui::ParamChoice*> (row.items.getFirst()) != nullptr;
        row.area = r.removeFromTop (choice ? 64 : 136);
        r.removeFromTop (10);
        auto inner = row.area.withTrimmedTop (28).reduced (12, 6);

        if (choice)
        {
            row.items.getFirst()->setBounds (inner.withHeight (26));
            continue;
        }

        for (auto* item : row.items)
            item->setBounds (inner.removeFromLeft (86).withSizeKeepingCentre (82, juce::jmin (100, inner.getHeight())));
    }
}

void MoreFxEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::colours::panel);

    for (auto& row : rows)
        fxui::drawSection (g, row.area, row.title, row.colour);

    g.setColour (theme::colours::muted);
    g.setFont (theme::ui (11.5f));
    g.drawFittedText (help, getLocalBounds().removeFromBottom (52).reduced (16, 6), juce::Justification::topLeft, 3);
}

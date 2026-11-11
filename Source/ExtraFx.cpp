#include "ExtraFx.h"

const char* DeEsserPlugin::xmlTypeName = "cotodeesser";
const char* MultibandPlugin::xmlTypeName = "cotomultiband";
const char* DistortionPlugin::xmlTypeName = "cotodrive";
const char* StereoShaperPlugin::xmlTypeName = "cotostereo";
const char* LoudnessMeterPlugin::xmlTypeName = "cotometer";
const char* TrackDelayPlugin::xmlTypeName = "cototrackdelay";

namespace
{
    juce::String ratioText (float v)     { return v >= 19.5f ? juce::String ("inf : 1") : juce::String (v, v < 10.0f ? 1 : 0) + " : 1"; }

    juce::String panText (float v)
    {
        const int p = juce::roundToInt (v * 100.0f);
        return p == 0 ? TRANS ("centre") : juce::String (std::abs (p)) + (p < 0 ? " L" : " R");
    }

    juce::String onOffText (float v)     { return v >= 0.5f ? TRANS ("On") : TRANS ("Off"); }

    float onePole (double sampleRate, float hz)
    {
        return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sampleRate);
    }
}

//==============================================================================
DeEsserPlugin::DeEsserPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    frequency = makeParam ("frequency", TRANS ("Frequency"), skewed (2000.0f, 12000.0f, 6000.0f), 6000.0f, hzText);
    threshold = makeParam ("threshold", TRANS ("Threshold"), { -60.0f, 0.0f }, -30.0f, dbText);
    range     = makeParam ("range",     TRANS ("Range"),     { 0.0f, 24.0f }, 10.0f, dbText);
    mode      = makeParam ("mode",      TRANS ("Mode"),      { 0.0f, 1.0f, 1.0f }, 0.0f,
                           [] (float v) { return v >= 0.5f ? TRANS ("Split") : TRANS ("Wide"); });
    listen    = makeParam ("listen",    TRANS ("Listen"),    { 0.0f, 1.0f, 1.0f }, 0.0f, onOffText);
}

DeEsserPlugin::~DeEsserPlugin()    { notifyListenersOfDeletion(); }

void DeEsserPlugin::reset()
{
    for (auto& f : filters)
        f.reset();

    follower.value = 0.0f;
}

void DeEsserPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };

    const float th = threshold->getCurrentValue(), maxCut = range->getCurrentValue();
    const bool split = mode->getCurrentValue() >= 0.5f, solo = listen->getCurrentValue() >= 0.5f;

    for (auto& f : filters)
        f.set (sampleRate, frequency->getCurrentValue(), 0.7071f);

    follower.setTimes (sampleRate, 0.5f, 60.0f);
    float most = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        float high[2] = {};
        float level = 0.0f;

        for (int c = 0; c < channels; ++c)
        {
            float low, band;
            filters[c].process (data[c][i], low, band, high[c]);
            level = juce::jmax (level, std::abs (high[c]));
        }

        // The highs over the threshold come down nearly all the way (at most by the range).
        const float envDb = juce::Decibels::gainToDecibels (follower.process (level), -120.0f);
        const float reduction = juce::jlimit (-maxCut, 0.0f, (th - envDb) * 0.8f);
        const float g = juce::Decibels::decibelsToGain (reduction);
        most = juce::jmin (most, reduction);

        for (int c = 0; c < channels; ++c)
        {
            const float x = data[c][i];
            data[c][i] = solo ? high[c] : split ? (x - high[c]) + high[c] * g : x * g;
        }
    }

    lastReduction = most;
}

//==============================================================================
MultibandPlugin::MultibandPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    static const char* const bandIDs[] = { "low", "mid", "high" };
    const juce::String bandNames[] = { TRANS ("Low"), TRANS ("Mid"), TRANS ("High") };

    for (int b = 0; b < 3; ++b)
    {
        const juce::String id (bandIDs[b]);
        threshold[b] = makeParam (id + "Thresh", bandNames[b] + " " + TRANS ("Threshold"), { -60.0f, 0.0f }, -20.0f, dbText);
        ratio[b]     = makeParam (id + "Ratio",  bandNames[b] + " " + TRANS ("Ratio"),     skewed (1.0f, 20.0f, 4.0f), 2.5f, ratioText);
        gain[b]      = makeParam (id + "Gain",   bandNames[b] + " " + TRANS ("Gain"),      { -12.0f, 12.0f }, 0.0f, dbText);
    }

    lowCross  = makeParam ("lowCross",  TRANS ("Low / mid"),  skewed (60.0f, 1000.0f, 200.0f), 200.0f, hzText);
    highCross = makeParam ("highCross", TRANS ("Mid / high"), skewed (1000.0f, 12000.0f, 3500.0f), 3500.0f, hzText);
    attack    = makeParam ("attack",    TRANS ("Attack"),     skewed (0.5f, 200.0f, 15.0f), 10.0f, msText);
    release   = makeParam ("release",   TRANS ("Release"),    skewed (10.0f, 2000.0f, 150.0f), 120.0f, msText);
}

MultibandPlugin::~MultibandPlugin()    { notifyListenersOfDeletion(); }

void MultibandPlugin::reset()
{
    for (int c = 0; c < 2; ++c)
    {
        lowSplit[c].reset();
        highSplit[c].reset();
        lowAlign[c].reset();
    }

    for (auto& f : followers)
        f.value = 0.0f;
}

void MultibandPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };

    const float lowHz = lowCross->getCurrentValue(), highHz = juce::jmax (highCross->getCurrentValue(), lowHz * 1.5f);

    for (int c = 0; c < 2; ++c)
    {
        lowSplit[c].set (sampleRate, lowHz);
        highSplit[c].set (sampleRate, highHz);
        lowAlign[c].set (sampleRate, highHz);
    }

    float th[3], r[3], makeup[3];

    for (int b = 0; b < 3; ++b)
    {
        followers[b].setTimes (sampleRate, attack->getCurrentValue(), release->getCurrentValue());
        th[b] = threshold[b]->getCurrentValue();
        r[b] = ratio[b]->getCurrentValue();
        makeup[b] = gain[b]->getCurrentValue();
    }

    for (int i = 0; i < n; ++i)
    {
        float bands[2][3] = {};
        float level[3] = {};

        for (int c = 0; c < channels; ++c)
        {
            // (The low band goes through the second split too, added back up, so the
            // three bands stay in step with each other.)
            float low, rest, alignedLow, alignedHigh;
            lowSplit[c].process (data[c][i], low, rest);
            highSplit[c].process (rest, bands[c][1], bands[c][2]);
            lowAlign[c].process (low, alignedLow, alignedHigh);
            bands[c][0] = alignedLow + alignedHigh;

            for (int b = 0; b < 3; ++b)
                level[b] = juce::jmax (level[b], std::abs (bands[c][b]));
        }

        float g[3];

        for (int b = 0; b < 3; ++b)
        {
            const float envDb = juce::Decibels::gainToDecibels (followers[b].process (level[b]), -120.0f);
            g[b] = juce::Decibels::decibelsToGain (morefx::compressDb (envDb, th[b], r[b], 6.0f) + makeup[b]);
        }

        for (int c = 0; c < channels; ++c)
            data[c][i] = bands[c][0] * g[0] + bands[c][1] * g[1] + bands[c][2] * g[2];
    }
}

//==============================================================================
juce::StringArray DistortionPlugin::typeNames()
{
    return { TRANS ("Warm"), TRANS ("Hard"), TRANS ("Fold") };
}

DistortionPlugin::DistortionPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    drive      = makeParam ("drive",      TRANS ("Drive"),      { 0.0f, 40.0f }, 12.0f, dbText);
    shape      = makeParam ("shape",      TRANS ("Shape"),      { 0.0f, 2.0f, 1.0f }, 0.0f,
                            [] (float v) { return typeNames()[juce::jlimit (0, 2, juce::roundToInt (v))]; });
    tone       = makeParam ("tone",       TRANS ("Tone"),       skewed (500.0f, 20000.0f, 5000.0f), 20000.0f, hzText);
    bits       = makeParam ("bits",       TRANS ("Bits"),       { 2.0f, 16.0f, 1.0f }, 16.0f,
                            [] (float v) { return juce::roundToInt (v) >= 16 ? TRANS ("Off") : juce::String (juce::roundToInt (v)); });
    downsample = makeParam ("downsample", TRANS ("Downsample"), { 1.0f, 40.0f, 1.0f }, 1.0f,
                            [] (float v) { return juce::roundToInt (v) <= 1 ? TRANS ("Off") : juce::String (juce::roundToInt (v)) + "x"; });
    mix        = makeParam ("mix",        TRANS ("Mix"),        { 0.0f, 1.0f }, 1.0f, pctText);
    output     = makeParam ("outVol",     TRANS ("Out"),        { -24.0f, 12.0f }, 0.0f, dbText);
}

DistortionPlugin::~DistortionPlugin()    { notifyListenersOfDeletion(); }

void DistortionPlugin::reset()
{
    toneState[0] = toneState[1] = held[0] = held[1] = 0.0f;
    holdCount = 0;
}

void DistortionPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };

    const float driveDb = drive->getCurrentValue();
    const float driveGain = juce::Decibels::decibelsToGain (driveDb);
    // Half of the drive is taken back afterwards, so turning it up doesn't just get louder.
    const float after = juce::Decibels::decibelsToGain (-0.5f * driveDb);
    const int kind = juce::jlimit (0, 2, juce::roundToInt (shape->getCurrentValue()));
    const int bitCount = juce::roundToInt (bits->getCurrentValue());
    const float levels = bitCount >= 16 ? 0.0f : std::pow (2.0f, (float) (bitCount - 1));
    const int hold = juce::jmax (1, juce::roundToInt (downsample->getCurrentValue()));
    const float toneCoef = juce::jlimit (0.0f, 1.0f, onePole (sampleRate, tone->getCurrentValue()));
    const float wet = mix->getCurrentValue(), outGain = juce::Decibels::decibelsToGain (output->getCurrentValue());

    for (int i = 0; i < n; ++i)
    {
        const bool fresh = holdCount == 0;
        holdCount = (holdCount + 1) % hold;

        for (int c = 0; c < channels; ++c)
        {
            const float dry = data[c][i];
            float y = dry * driveGain;

            if (kind == 0)       y = std::tanh (y);
            else if (kind == 1)  y = juce::jlimit (-1.0f, 1.0f, y);
            else                 y = std::sin (y * juce::MathConstants<float>::halfPi);

            y *= after;

            if (levels > 0.0f)
                y = std::round (y * levels) / levels;

            if (fresh)  held[c] = y;
            else        y = held[c];

            toneState[c] += (y - toneState[c]) * toneCoef;
            JUCE_UNDENORMALISE (toneState[c]);
            data[c][i] = (dry * (1.0f - wet) + toneState[c] * wet) * outGain;
        }
    }
}

//==============================================================================
juce::StringArray StereoShaperPlugin::channelNames()
{
    return { TRANS ("Normal"), TRANS ("Swap L / R"), TRANS ("Flip left"), TRANS ("Flip right") };
}

StereoShaperPlugin::StereoShaperPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    width    = makeParam ("width",    TRANS ("Width"),     { 0.0f, 2.0f }, 1.0f, pctText);
    delay    = makeParam ("delay",    TRANS ("Delay"),     { 0.0f, 30.0f }, 0.0f,
                          [] (float v) { return v < 0.05f ? TRANS ("Off") : juce::String (v, 1) + " ms"; });
    bassMono = makeParam ("bassMono", TRANS ("Bass mono"), { 0.0f, 400.0f }, 0.0f,
                          [] (float v) { return v < 20.0f ? TRANS ("Off") : juce::String (juce::roundToInt (v)) + " Hz"; });
    pan      = makeParam ("pan",      TRANS ("Pan"),       { -1.0f, 1.0f }, 0.0f, panText);
    output   = makeParam ("outVol",   TRANS ("Out"),       { -24.0f, 12.0f }, 0.0f, dbText);
    channels = makeParam ("chanMode", TRANS ("Channels"),  { 0.0f, 3.0f, 1.0f }, 0.0f,
                          [] (float v) { return channelNames()[juce::jlimit (0, 3, juce::roundToInt (v))]; });
}

StereoShaperPlugin::~StereoShaperPlugin()    { notifyListenersOfDeletion(); }

void StereoShaperPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
    line.assign ((size_t) (sampleRate * 0.05) + 8, 0.0f);
    reset();
}

void StereoShaperPlugin::reset()
{
    std::fill (line.begin(), line.end(), 0.0f);
    writePos = 0;
    bassFilter.reset();
}

void StereoShaperPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0 || line.empty())
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, numChannels = juce::jmin (2, buffer.getNumChannels());
    float* data[2] = { buffer.getWritePointer (0, fc.bufferStartSample),
                       buffer.getWritePointer (numChannels > 1 ? 1 : 0, fc.bufferStartSample) };

    const int size = (int) line.size();
    const float w = width->getCurrentValue();
    const int delaySamples = juce::jlimit (0, size - 1, juce::roundToInt (delay->getCurrentValue() * 0.001f * (float) sampleRate));
    const float bassHz = bassMono->getCurrentValue();
    const bool keepBassCentred = bassHz >= 20.0f;
    const int kind = juce::jlimit (0, 3, juce::roundToInt (channels->getCurrentValue()));
    const float p = pan->getCurrentValue(), outGain = juce::Decibels::decibelsToGain (output->getCurrentValue());
    const float gainL = outGain * juce::jmin (1.0f, 1.0f - p), gainR = outGain * juce::jmin (1.0f, 1.0f + p);

    if (keepBassCentred)
        bassFilter.set (sampleRate, bassHz, 0.7071f);

    for (int i = 0; i < n; ++i)
    {
        float l = data[0][i], r = numChannels > 1 ? data[1][i] : l;

        if (kind == 1)       std::swap (l, r);
        else if (kind == 2)  l = -l;
        else if (kind == 3)  r = -r;

        const float mid = 0.5f * (l + r);
        float side = 0.5f * (l - r);

        if (keepBassCentred)
        {
            float low, band, high;
            bassFilter.process (side, low, band, high);
            side = high;
        }

        side *= w;
        l = mid + side;
        r = mid - side;

        // The right side a little late (Haas): a sound in the middle spreads out.
        line[(size_t) writePos] = r;
        r = line[(size_t) ((writePos - delaySamples + size) % size)];
        writePos = (writePos + 1) % size;

        data[0][i] = l * gainL;

        if (numChannels > 1)
            data[1][i] = r * gainR;
    }
}

//==============================================================================
LoudnessMeterPlugin::LoudnessMeterPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    histogram.assign ((size_t) numBins, 0);
}

LoudnessMeterPlugin::~LoudnessMeterPlugin()    { notifyListenersOfDeletion(); }

void LoudnessMeterPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 44100.0;
    slotLength = juce::jmax (1, juce::roundToInt (sampleRate * 0.1));

    // The "K" weighting of the loudness standard (ITU-R BS.1770): a high shelf (how a head
    // changes the sound) and a low cut, worked out for this sample rate.
    {
        const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double vh = std::pow (10.0, gainDb / 20.0), vb = std::pow (vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;

        for (auto& s : shelf)
        {
            s.b0 = (vh + vb * k / q + k * k) / a0;
            s.b1 = 2.0 * (k * k - vh) / a0;
            s.b2 = (vh - vb * k / q + k * k) / a0;
            s.a1 = 2.0 * (k * k - 1.0) / a0;
            s.a2 = (1.0 - k / q + k * k) / a0;
        }
    }

    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const double a0 = 1.0 + k / q + k * k;

        for (auto& h : highPass)
        {
            h.b0 = 1.0;
            h.b1 = -2.0;
            h.b2 = 1.0;
            h.a1 = 2.0 * (k * k - 1.0) / a0;
            h.a2 = (1.0 - k / q + k * k) / a0;
        }
    }

    reset();
}

void LoudnessMeterPlugin::reset()
{
    for (int c = 0; c < 2; ++c)
        shelf[c].z1 = shelf[c].z2 = highPass[c].z1 = highPass[c].z2 = 0.0;

    clearMeasurement();
}

void LoudnessMeterPlugin::clearMeasurement()
{
    std::fill (std::begin (slots), std::end (slots), 0.0);
    std::fill (histogram.begin(), histogram.end(), 0);
    slotIndex = slotsFilled = slotSamples = 0;
    slotSum = 0.0;
    peakGain = 0.0f;
    momentary = silence;
    shortTerm = silence;
    integrated = silence;
    peak = silence;
}

void LoudnessMeterPlugin::finishSlot()
{
    slots[slotIndex] = slotSum / (double) slotLength;
    slotIndex = (slotIndex + 1) % numSlots;
    slotsFilled = juce::jmin (numSlots, slotsFilled + 1);
    slotSum = 0.0;
    slotSamples = 0;

    auto meanOfLast = [this] (int count)
    {
        count = juce::jmin (count, slotsFilled);
        double total = 0.0;

        for (int k = 1; k <= count; ++k)
            total += slots[(slotIndex - k + numSlots) % numSlots];

        return count > 0 ? total / count : 0.0;
    };

    auto toLufs = [] (double energy)
    {
        return energy > 1.0e-10 ? (float) (-0.691 + 10.0 * std::log10 (energy)) : silence;
    };

    const float now = toLufs (meanOfLast (4));
    momentary = now;
    shortTerm = toLufs (meanOfLast (numSlots));

    if (slotsFilled < 4)
        return;

    // Integrated: the average of every 0.4 s piece louder than -70 LUFS, then again of
    // only those within 10 below that average (so pauses don't pull the number down).
    if (now > -70.0f)
        ++histogram[(size_t) juce::jlimit (0, numBins - 1, (int) ((now + 70.0f) * 10.0f))];

    auto binLufs = [] (int bin)        { return -70.0 + (bin + 0.5) * 0.1; };
    auto binEnergy = [&] (int bin)     { return std::pow (10.0, (binLufs (bin) + 0.691) / 10.0); };

    auto average = [&] (double from)
    {
        double total = 0.0;
        juce::int64 count = 0;

        for (int b = 0; b < numBins; ++b)
            if (histogram[(size_t) b] > 0 && binLufs (b) >= from)
            {
                total += binEnergy (b) * histogram[(size_t) b];
                count += histogram[(size_t) b];
            }

        return count > 0 ? total / (double) count : 0.0;
    };

    const float all = toLufs (average (-70.0));
    integrated = all <= silence ? silence : toLufs (average ((double) all - 10.0));
}

void LoudnessMeterPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    if (restart.exchange (false))
        clearMeasurement();

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    const float* data[2] = { buffer.getReadPointer (0, fc.bufferStartSample),
                             buffer.getReadPointer (channels > 1 ? 1 : 0, fc.bufferStartSample) };

    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < channels; ++c)
        {
            const float x = data[c][i];
            peakGain = juce::jmax (peakGain, std::abs (x));
            const double y = highPass[c].process (shelf[c].process ((double) x));
            slotSum += y * y;
        }

        if (++slotSamples >= slotLength)
            finishSlot();
    }

    peak = juce::Decibels::gainToDecibels (peakGain, silence);
}

//==============================================================================
TrackDelayPlugin::TrackDelayPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    time = makeParam ("time", TRANS ("Delay"), skewed (0.0f, maxMs, 60.0f), 0.0f,
                      [] (float v) { return v < 0.05f ? TRANS ("Off") : juce::String (v, v < 20.0f ? 1 : 0) + " ms"; });
}

TrackDelayPlugin::~TrackDelayPlugin()    { notifyListenersOfDeletion(); }

void TrackDelayPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 44100.0;
    line.setSize (2, (int) (sampleRate * maxMs * 0.001) + 8);
    reset();
}

void TrackDelayPlugin::reset()
{
    line.clear();
    writePos = 0;
}

void TrackDelayPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0 || line.getNumSamples() == 0)
        return;

    auto& buffer = *fc.destBuffer;
    const int n = fc.bufferNumSamples, channels = juce::jmin (2, buffer.getNumChannels());
    const int size = line.getNumSamples();
    const int delaySamples = juce::jlimit (0, size - 1, juce::roundToInt (time->getCurrentValue() * 0.001f * (float) sampleRate));

    for (int c = 0; c < channels; ++c)
    {
        auto* data = buffer.getWritePointer (c, fc.bufferStartSample);
        auto* stored = line.getWritePointer (c);
        int w = writePos;

        for (int i = 0; i < n; ++i)
        {
            stored[w] = data[i];
            data[i] = stored[(w - delaySamples + size) % size];
            w = (w + 1) % size;
        }
    }

    writePos = (writePos + n) % size;
}

//==============================================================================
LoudnessMeterEditor::LoudnessMeterEditor (LoudnessMeterPlugin& p) : plugin (p)
{
    restartButton.setButtonText (TRANS ("Restart"));
    restartButton.setTooltip (TRANS ("Start measuring again: clears Integrated and Peak"));
    restartButton.onClick = [this] { plugin.restartMeasuring(); };
    addAndMakeVisible (restartButton);
    setSize (420, 250);
    startTimerHz (15);
}

void LoudnessMeterEditor::resized()
{
    restartButton.setBounds (getLocalBounds().reduced (12).removeFromTop (150).removeFromBottom (36).removeFromRight (96).reduced (8, 5));
}

void LoudnessMeterEditor::paint (juce::Graphics& g)
{
    using namespace theme::colours;
    g.fillAll (panel);

    auto r = getLocalBounds().reduced (12);
    auto top = r.removeFromTop (150);
    fxui::drawSection (g, top, TRANS ("Loudness"), teal);

    const float values[] = { plugin.momentary.load(), plugin.shortTerm.load(), plugin.integrated.load(), plugin.peak.load() };
    const juce::String names[] = { TRANS ("Now"), TRANS ("Last 3 s"), TRANS ("Whole song"), TRANS ("Peak") };
    const juce::String units[] = { "LUFS", "LUFS", "LUFS", "dB" };
    const juce::Colour tints[] = { lcdText, lcdText, amber, plugin.peak.load() > -0.1f ? red : lcdText };

    auto tiles = top.withTrimmedTop (28).withTrimmedBottom (40).reduced (10, 0);
    const int tileWidth = tiles.getWidth() / 4;

    for (int i = 0; i < 4; ++i)
    {
        auto tile = tiles.removeFromLeft (tileWidth).reduced (3, 0);
        g.setColour (lcd);
        g.fillRoundedRectangle (tile.toFloat(), 5.0f);
        g.setColour (muted);
        g.setFont (theme::ui (10.5f, theme::Weight::semibold));
        g.drawText (fxui::upper (names[i]), tile.removeFromTop (20), juce::Justification::centred, false);
        g.setColour (lcdMuted);
        g.setFont (theme::ui (10.0f));
        g.drawText (units[i], tile.removeFromBottom (18), juce::Justification::centred, false);
        g.setColour (tints[i]);
        g.setFont (theme::mono (20.0f, true));
        g.drawText (values[i] <= LoudnessMeterPlugin::silence + 0.5f ? juce::String ("--") : juce::String (values[i], 1),
                    tile, juce::Justification::centred, false);
    }

    // "Now" as a bar from -40 to 0 LUFS, with a mark at -14 (where streaming services aim).
    auto bar = top.removeFromBottom (36).reduced (16, 12).withTrimmedRight (96).toFloat();
    g.setColour (lcd);
    g.fillRoundedRectangle (bar, 3.0f);
    const float amount = juce::jlimit (0.0f, 1.0f, (values[0] + 40.0f) / 40.0f);
    g.setColour (values[0] > -9.0f ? orange : teal);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * amount), 3.0f);
    g.setColour (amber);
    g.fillRect (bar.getX() + bar.getWidth() * (26.0f / 40.0f), bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f);

    g.setColour (muted);
    g.setFont (theme::ui (11.5f));
    g.drawFittedText (TRANS ("Changes nothing in the sound. Put it last on the Master, play the whole song and read \"Whole song\": "
                             "streaming services turn songs down to about -14 LUFS (the amber mark). Keep Peak under 0 dB."),
                      r.reduced (4, 8), juce::Justification::topLeft, 4);
}

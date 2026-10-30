#include "SamplerPlugin.h"

const char* SamplerPlugin::xmlTypeName = "cotosampler";
const juce::Identifier SamplerPlugin::assetID ("asset"), SamplerPlugin::cutID ("cutItself"), SamplerPlugin::noteLengthID ("noteLength");

namespace
{
    juce::String timeText (float ms)
    {
        if (ms >= 1000.0f)  return juce::String (ms / 1000.0f, 2) + " s";
        if (ms >= 10.0f)    return juce::String (juce::roundToInt (ms)) + " ms";
        return juce::String (ms, 1) + " ms";
    }

    juce::String onOff (float v)    { return v >= 0.5f ? TRANS ("On") : TRANS ("Off"); }
}

SamplerPlugin::SamplerPlugin (te::PluginCreationInfo info) : CotoFxPlugin (info)
{
    cutValue.referTo (state, cutID, getUndoManager(), false);
    noteLengthValue.referTo (state, noteLengthID, getUndoManager(), false);

    for (auto& p : voicePositions)
        p = -1.0f;

    // Where in the sound: shown as a time once the sound is loaded.
    auto position = [this] (float v)
    {
        const double s = v * getSampleSeconds();

        if (getSampleSeconds() <= 0.0)
            return pctText (v);

        return s < 1.0 ? juce::String (juce::roundToInt (s * 1000.0)) + " ms" : juce::String (s, 2) + " s";
    };

    // (The first two existed before and keep their IDs: songs and automation clips still find them.)
    tuneParam  = makeParam ("tune", TRANS ("Tune"), { -24.0f, 24.0f, 1.0f }, 0.0f,
                            [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st"; });
    levelParam = makeParam ("level", TRANS ("Level"), { 0.0f, 1.0f }, 1.0f,
                            [] (float v) { return v <= 0.0001f ? juce::String ("-inf") : dbText (juce::Decibels::gainToDecibels (v)); });
    fineParam  = makeParam ("fine", TRANS ("Fine"), { -100.0f, 100.0f, 1.0f }, 0.0f,
                            [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct"; });

    startParam   = makeParam ("start", TRANS ("Start"), { 0.0f, 1.0f }, 0.0f, position);
    endParam     = makeParam ("end", TRANS ("End"), { 0.0f, 1.0f }, 1.0f, position);
    fadeInParam  = makeParam ("fadeIn", TRANS ("Fade in"), skewed (0.0f, 2000.0f, 150.0f), 0.0f, timeText);
    fadeOutParam = makeParam ("fadeOut", TRANS ("Fade out"), skewed (0.0f, 2000.0f, 150.0f), 0.0f, timeText);
    reverseParam = makeParam ("reverse", TRANS ("Reverse"), { 0.0f, 1.0f, 1.0f }, 0.0f, onOff);

    envOnParam   = makeParam ("envOn", TRANS ("Envelope"), { 0.0f, 1.0f, 1.0f }, 0.0f, onOff);
    attackParam  = makeParam ("attack", TRANS ("Attack"), skewed (0.0f, 5000.0f, 300.0f), 0.0f, timeText);
    holdParam    = makeParam ("hold", TRANS ("Hold"), skewed (0.0f, 5000.0f, 300.0f), 0.0f, timeText);
    decayParam   = makeParam ("decay", TRANS ("Decay"), skewed (1.0f, 10000.0f, 600.0f), 400.0f, timeText);
    sustainParam = makeParam ("sustain", TRANS ("Sustain"), { 0.0f, 1.0f }, 1.0f, pctText);
    releaseParam = makeParam ("release", TRANS ("Release"), skewed (1.0f, 10000.0f, 600.0f), 60.0f, timeText);

    filterTypeParam = makeParam ("filterType", TRANS ("Filter"), { 0.0f, 3.0f, 1.0f }, 0.0f, [] (float v)
    {
        const int t = juce::roundToInt (v);
        return t == 1 ? TRANS ("Low pass") : t == 2 ? TRANS ("High pass") : t == 3 ? TRANS ("Band pass") : TRANS ("Off");
    });
    cutoffParam    = makeParam ("cutoff", TRANS ("Cutoff"), skewed (20.0f, 20000.0f, 1000.0f), 20000.0f, hzText);
    resonanceParam = makeParam ("resonance", TRANS ("Resonance"), { 0.0f, 1.0f }, 0.0f, pctText);
}

SamplerPlugin::~SamplerPlugin()
{
    notifyListenersOfDeletion();
}

void SamplerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate > 0 ? info.sampleRate : 44100.0;
    reset();
}

void SamplerPlugin::reset()
{
    for (auto& v : voices)
        v.active = false;

    for (auto& p : voicePositions)
        p = -1.0f;

    for (int c = 0; c < 2; ++c)
        ic1[c] = ic2[c] = 0.0f;
}

void SamplerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    CotoFxPlugin::restorePluginStateFromValueTree (v);
    te::copyPropertiesToCachedValues (v, cutValue, noteLengthValue);

    if (v.hasProperty (assetID))
        state.setProperty (assetID, v[assetID], nullptr);
}

void SamplerPlugin::loadSample (const juce::String& hash, const juce::File& file)
{
    const bool present = hash.isNotEmpty() && file.existsAsFile();

    if (hash == loadedHash && present == loadedFromFile)
        return;

    std::unique_ptr<Sample> fresh;

    if (present)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) })
        {
            const auto length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * maxSeconds));
            const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);

            if (length > 1 && reader->sampleRate > 0.0)
            {
                fresh = std::make_unique<Sample>();
                fresh->rate = reader->sampleRate;
                fresh->audio.setSize (channels, length);
                reader->read (&fresh->audio, 0, length, 0, true, channels > 1);
            }
        }
    }

    // The outline for the editor (min / max per bin, both sides together).
    std::vector<float> outline;

    if (fresh != nullptr)
    {
        const int length = fresh->audio.getNumSamples();
        outline.assign ((size_t) peakBins * 2, 0.0f);

        for (int b = 0; b < peakBins; ++b)
        {
            const int from = (int) ((juce::int64) length * b / peakBins);
            const int to = juce::jmax (from + 1, (int) ((juce::int64) length * (b + 1) / peakBins));
            float lo = 0.0f, hi = 0.0f;

            for (int c = 0; c < fresh->audio.getNumChannels(); ++c)
            {
                auto r = juce::FloatVectorOperations::findMinAndMax (fresh->audio.getReadPointer (c, from), juce::jmin (to, length) - from);
                lo = juce::jmin (lo, r.getStart());
                hi = juce::jmax (hi, r.getEnd());
            }

            outline[(size_t) b * 2] = lo;
            outline[(size_t) b * 2 + 1] = hi;
        }
    }

    loadedHash = hash;
    loadedFromFile = fresh != nullptr;
    sampleSeconds = fresh != nullptr ? fresh->audio.getNumSamples() / fresh->rate : 0.0;
    peaks = std::move (outline);

    {
        const juce::ScopedLock sl (sampleLock);
        std::swap (sample, fresh);   // the old sound is freed below, outside the lock
        reset();
    }
}

//==============================================================================
SamplerPlugin::Settings SamplerPlugin::readSettings (const Sample& s) const
{
    Settings k;
    k.reverse = reverseParam->getCurrentValue() >= 0.5f;
    k.region = regionOf (s.audio.getNumSamples(), startParam->getCurrentValue(), endParam->getCurrentValue(), k.reverse);
    k.fadeIn = fadeInParam->getCurrentValue() * 0.001 * s.rate;
    k.fadeOut = fadeOutParam->getCurrentValue() * 0.001 * s.rate;
    k.tune = tuneParam->getCurrentValue() + fineParam->getCurrentValue() * 0.01f;
    k.level = levelParam->getCurrentValue();
    k.stopAtNoteEnd = noteLengthValue.get();

    k.envelope = envOnParam->getCurrentValue() >= 0.5f;
    const auto samples = [this] (te::AutomatableParameter* p) { return (float) (p->getCurrentValue() * 0.001 * sampleRate); };
    k.sustain = juce::jlimit (0.0f, 1.0f, sustainParam->getCurrentValue());
    k.attackStep = 1.0f / juce::jmax (1.0f, samples (attackParam));
    k.holdSamples = (int) samples (holdParam);
    k.decayStep = (1.0f - k.sustain) / juce::jmax (1.0f, samples (decayParam));
    k.releaseSamples = juce::jmax (1.0f, samples (releaseParam));

    k.filterType = juce::roundToInt (filterTypeParam->getCurrentValue());
    k.cutoff = juce::jlimit (20.0f, (float) (sampleRate * 0.45), cutoffParam->getCurrentValue());
    k.resonance = juce::jlimit (0.0f, 1.0f, resonanceParam->getCurrentValue());
    return k;
}

void SamplerPlugin::startVoice (const Sample& s, const Settings& k, int noteNumber, float velocity, bool cut,
                                float pan, float fineCents)
{
    if (k.region.getLength() < 2.0)
        return;   // trimmed to nothing

    if (cut)
        for (auto& v : voices)
            if (v.active && v.fadeStep == 0.0f)
                v.fadeStep = 1.0f / (float) (0.005 * sampleRate);   // a 5 ms fade, no click

    auto& v = voices[(size_t) nextVoice];
    nextVoice = (nextVoice + 1) % maxVoices;

    const double speed = s.rate / sampleRate * std::pow (2.0, (noteNumber - 60 + (double) k.tune + fineCents / 100.0) / 12.0);
    v.active = true;
    v.released = false;
    v.note = noteNumber;
    v.step = k.reverse ? -speed : speed;
    v.position = v.startPosition = k.reverse ? k.region.getEnd() - 1.0 - 1.0e-6 : k.region.getStart();
    v.gain = velocity;
    // The note's pan (FL's note property): a balance, the far side turned down.
    v.gainL = juce::jlimit (0.0f, 1.0f, 1.0f - pan);
    v.gainR = juce::jlimit (0.0f, 1.0f, 1.0f + pan);
    v.fade = 1.0f;
    v.fadeStep = 0.0f;
    v.useEnvelope = k.envelope;
    v.stage = Stage::attack;
    v.env = k.envelope ? 0.0f : 1.0f;
    v.holdLeft = k.holdSamples;
}

void SamplerPlugin::releaseNote (int noteNumber, const Settings& k, int releaseVelocity)
{
    // The note's release velocity (FL's "release"): 64 = the Release knob's time, lower =
    // shorter (down to a quarter), higher = longer (up to 4 times). 0 = not given (live keys).
    const float factor = releaseVelocity <= 0 ? 1.0f
                                              : juce::jlimit (0.25f, 4.0f, std::pow (2.0f, (float) (releaseVelocity - 64) / 32.0f));

    // With the envelope on: its release. With "Stop at note end": a short fade (no click).
    // Otherwise the whole sound plays, like before (one-shot).
    for (auto& v : voices)
    {
        if (! v.active || v.released || v.note != noteNumber)
            continue;

        if (v.useEnvelope)
        {
            v.released = true;
            v.stage = Stage::release;
            v.releaseStep = juce::jmax (1.0e-4f, v.env) / (k.releaseSamples * factor);
        }
        else if (k.stopAtNoteEnd)
        {
            v.released = true;

            if (v.fadeStep == 0.0f)
                v.fadeStep = 1.0f / (float) (0.01 * sampleRate);   // 10 ms
        }
    }
}

float SamplerPlugin::nextEnvelope (Voice& v, const Settings& k)
{
    if (! v.useEnvelope)
        return 1.0f;

    switch (v.stage)
    {
        case Stage::attack:
            if ((v.env += k.attackStep) >= 1.0f)
            {
                v.env = 1.0f;
                v.stage = Stage::hold;
            }
            break;

        case Stage::hold:
            if (--v.holdLeft <= 0)
                v.stage = Stage::decay;
            break;

        case Stage::decay:
            if ((v.env -= k.decayStep) <= k.sustain)
            {
                v.env = k.sustain;
                v.stage = Stage::sustain;
            }
            break;

        case Stage::sustain:
            v.env = k.sustain;
            break;

        case Stage::release:
            if ((v.env -= v.releaseStep) <= 0.0f)
            {
                v.env = 0.0f;
                v.active = false;
            }
            break;
    }

    return v.env;
}

void SamplerPlugin::updateFilterCoefficients (float resonance)
{
    // Topology-preserving state-variable filter (Zavalishin): stable while the cutoff moves.
    const float g = std::tan (juce::MathConstants<float>::pi * smoothedCutoff / (float) sampleRate);
    const float q = 0.5f + resonance * resonance * 14.5f;   // 0.5 (soft) .. 15 (ringing)
    fk = 1.0f / q;
    fa1 = 1.0f / (1.0f + g * (g + fk));
    fa2 = g * fa1;
    fa3 = g * fa2;
}

void SamplerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    auto& buffer = *fc.destBuffer;
    const int start = fc.bufferStartSample, num = fc.bufferNumSamples;
    buffer.clear (start, num);

    // The message thread is swapping the sound right now: skip this one block.
    const juce::ScopedTryLock sl (sampleLock);

    if (! sl.isLocked())
        return;

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        reset();

    if (sample == nullptr)
        return;

    const auto& s = *sample;
    const auto k = readSettings (s);
    const bool cut = cutValue.get();

    // Notes starting and ending in this block, in order (no allocation here: a fixed list).
    // At the same moment an end goes before a start, so a note that ends where the next
    // one starts doesn't release the new one; a note's pan / fine pitch (key pressure on
    // channel 16 / 15, see PatternCompiler::notePropertiesInto) goes just before its start.
    enum Kind { noteOff = 0, panOf = 1, fineOf = 2, noteOn = 3 };
    struct Event { int pos, note, order, kind; float vel; int raw; };
    std::array<Event, 512> events;
    int numEvents = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (auto& m : *fc.bufferForMidiMessages)
        {
            int kind = -1;

            if (m.isNoteOn())                                         kind = noteOn;
            else if (m.isNoteOff())                                   kind = noteOff;
            else if (m.isAftertouch() && m.getChannel() == 16)        kind = panOf;
            else if (m.isAftertouch() && m.getChannel() == 15)        kind = fineOf;

            if (numEvents >= (int) events.size() || kind < 0)
                continue;

            const int raw = kind == noteOn || kind == noteOff ? (int) m.getVelocity() : m.getAfterTouchValue();
            events[(size_t) numEvents] = { juce::jlimit (start, start + num - 1, juce::roundToInt (m.getTimeStamp() * sampleRate)),
                                           m.getNoteNumber(), numEvents, kind, m.getFloatVelocity(), raw };
            ++numEvents;
        }

        std::sort (events.begin(), events.begin() + numEvents, [] (const Event& a, const Event& b)
        {
            if (a.pos != b.pos)     return a.pos < b.pos;
            if (a.kind != b.kind)   return a.kind < b.kind;
            return a.order < b.order;
        });
    }

    // Pan and fine pitch waiting for their note (only for a note starting at that moment).
    std::array<float, 128> pendingPan {}, pendingFine {};
    std::array<int, 128> pendingAt;
    pendingAt.fill (-1);

    if (k.filterType != lastFilterType)
    {
        for (int c = 0; c < 2; ++c)
            ic1[c] = ic2[c] = 0.0f;

        if (lastFilterType == 0)
            smoothedCutoff = k.cutoff;   // switched on: start where the knob is

        lastFilterType = k.filterType;
        coefficientCountdown = 0;
    }

    const int length = s.audio.getNumSamples();
    const float* srcL = s.audio.getReadPointer (0);
    const float* srcR = s.audio.getNumChannels() > 1 ? s.audio.getReadPointer (1) : srcL;
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    const double first = k.region.getStart(), last = juce::jmin ((double) length, k.region.getEnd());
    const float cutoffSmoothing = 1.0f - std::exp (-1.0f / (0.01f * (float) sampleRate));   // ~10 ms
    int eventIndex = 0;

    for (int i = start; i < start + num; ++i)
    {
        while (eventIndex < numEvents && events[(size_t) eventIndex].pos <= i)
        {
            auto& ev = events[(size_t) eventIndex++];
            const auto key = (size_t) juce::jlimit (0, 127, ev.note);

            if (ev.kind == panOf || ev.kind == fineOf)
            {
                if (pendingAt[key] != ev.pos)
                {
                    pendingPan[key] = pendingFine[key] = 0.0f;
                    pendingAt[key] = ev.pos;
                }

                const float amount = juce::jlimit (-1.0f, 1.0f, (float) (ev.raw - 64) / 63.0f);
                (ev.kind == panOf ? pendingPan : pendingFine)[key] = ev.kind == panOf ? amount : amount * 100.0f;
            }
            else if (ev.kind == noteOn)
            {
                const bool hasProperties = pendingAt[key] == ev.pos;
                startVoice (s, k, ev.note, ev.vel, cut, hasProperties ? pendingPan[key] : 0.0f, hasProperties ? pendingFine[key] : 0.0f);
                pendingAt[key] = -1;
            }
            else
            {
                releaseNote (ev.note, k, ev.raw);
            }
        }

        float outL = 0.0f, outR = 0.0f;

        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            const int index = (int) std::floor (v.position);

            if (v.position < first || index + 1 >= (int) last)
            {
                v.active = false;
                continue;
            }

            // Fades at both ends of the trimmed sound, in the order it plays.
            float g = v.gain * v.fade * k.level;
            const double played = std::abs (v.position - v.startPosition);
            const double remaining = v.step > 0.0 ? last - 1.0 - v.position : v.position - first;

            if (k.fadeIn > 0.0 && played < k.fadeIn)    g *= (float) (played / k.fadeIn);
            if (k.fadeOut > 0.0 && remaining < k.fadeOut) g *= (float) juce::jmax (0.0, remaining / k.fadeOut);

            g *= nextEnvelope (v, k);

            const float frac = (float) (v.position - index);
            outL += (srcL[index] + frac * (srcL[index + 1] - srcL[index])) * g * v.gainL;
            outR += (srcR[index] + frac * (srcR[index + 1] - srcR[index])) * g * v.gainR;
            v.position += v.step;

            if (v.fadeStep > 0.0f && (v.fade -= v.fadeStep) <= 0.0f)
                v.active = false;
        }

        if (k.filterType > 0)
        {
            smoothedCutoff += (k.cutoff - smoothedCutoff) * cutoffSmoothing;

            if (--coefficientCountdown <= 0)
            {
                updateFilterCoefficients (k.resonance);
                coefficientCountdown = 16;
            }

            float* out[2] = { &outL, &outR };

            for (int c = 0; c < 2; ++c)
            {
                const float x = *out[c];
                const float v3 = x - ic2[c];
                const float v1 = fa1 * ic1[c] + fa2 * v3;
                const float v2 = ic2[c] + fa2 * ic1[c] + fa3 * v3;
                ic1[c] = 2.0f * v1 - ic1[c];
                ic2[c] = 2.0f * v2 - ic2[c];
                *out[c] = k.filterType == 1 ? v2 : k.filterType == 2 ? x - fk * v1 - v2 : fk * v1;   // (band pass: 0 dB at its centre)
            }
        }

        if (right != nullptr)
        {
            left[i] += outL;
            right[i] += outR;
        }
        else
        {
            left[i] += 0.5f * (outL + outR);
        }
    }

    // Where the voices are, for the editor's waveform (0..1 in the order the sound plays).
    for (int i = 0; i < maxVoices; ++i)
    {
        const auto& v = voices[(size_t) i];
        const float f = v.active ? (float) (v.position / juce::jmax (1, length)) : -1.0f;
        voicePositions[(size_t) i] = v.active && k.reverse ? 1.0f - f : f;
    }
}

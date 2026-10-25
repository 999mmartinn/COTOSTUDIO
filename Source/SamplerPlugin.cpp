#include "SamplerPlugin.h"

const char* SamplerPlugin::xmlTypeName = "cotosampler";
const juce::Identifier SamplerPlugin::assetID ("asset"), SamplerPlugin::cutID ("cutItself");

static const juce::Identifier tuneID ("tune"), levelID ("level");

SamplerPlugin::SamplerPlugin (te::PluginCreationInfo info) : Plugin (info)
{
    auto um = getUndoManager();
    tuneValue.referTo  (state, tuneID,  um, 0.0f);
    levelValue.referTo (state, levelID, um, 1.0f);
    cutValue.referTo   (state, cutID,   um, false);

    tuneParam  = addParam ("tune",  TRANS("Tune"),  { -24.0f, 24.0f, 1.0f },
                           [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st"; },
                           [] (const juce::String& s) { return (float) s.getIntValue(); });
    levelParam = addParam ("level", TRANS("Level"), { 0.0f, 1.0f });

    tuneParam->attachToCurrentValue (tuneValue);
    levelParam->attachToCurrentValue (levelValue);
}

SamplerPlugin::~SamplerPlugin()
{
    notifyListenersOfDeletion();

    for (auto p : { tuneParam, levelParam })
        p->detachFromCurrentValue();
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
}

void SamplerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, tuneValue, levelValue, cutValue);

    if (v.hasProperty (assetID))
        state.setProperty (assetID, v[assetID], nullptr);

    for (auto p : getAutomatableParameters())
        p->updateFromAttachedValue();
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

    loadedHash = hash;
    loadedFromFile = fresh != nullptr;
    sampleSeconds = fresh != nullptr ? fresh->audio.getNumSamples() / fresh->rate : 0.0;

    {
        const juce::ScopedLock sl (sampleLock);
        std::swap (sample, fresh);   // the old sound is freed below, outside the lock
        reset();
    }
}

void SamplerPlugin::startVoice (const Sample& s, int noteNumber, float velocity, float tune, bool cut)
{
    if (cut)
        for (auto& v : voices)
            if (v.active && v.fadeStep == 0.0f)
                v.fadeStep = 1.0f / (float) (0.005 * sampleRate);   // a 5 ms fade, no click

    auto& v = voices[(size_t) nextVoice];
    nextVoice = (nextVoice + 1) % maxVoices;

    v.active = true;
    v.position = 0.0;
    v.gain = velocity;
    v.fade = 1.0f;
    v.fadeStep = 0.0f;
    v.step = s.rate / sampleRate * std::pow (2.0, (noteNumber - 60 + (double) tune) / 12.0);
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
    const float tune = tuneParam->getCurrentValue();
    const float level = levelParam->getCurrentValue();
    const bool cut = cutValue.get();

    // Note-ons with their sample position inside this block.
    struct Hit { int pos; int note; float vel; };
    juce::Array<Hit> hits;

    if (fc.bufferForMidiMessages != nullptr)
        for (auto& m : *fc.bufferForMidiMessages)
            if (m.isNoteOn())
                hits.add ({ juce::jlimit (start, start + num - 1, juce::roundToInt (m.getTimeStamp() * sampleRate)),
                            m.getNoteNumber(), m.getFloatVelocity() });

    const int length = s.audio.getNumSamples();
    const float* srcL = s.audio.getReadPointer (0);
    const float* srcR = s.audio.getNumChannels() > 1 ? s.audio.getReadPointer (1) : srcL;
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;
    int hitIndex = 0;

    for (int i = start; i < start + num; ++i)
    {
        while (hitIndex < hits.size() && hits.getReference (hitIndex).pos <= i)
        {
            auto& h = hits.getReference (hitIndex++);
            startVoice (s, h.note, h.vel, tune, cut);
        }

        float outL = 0.0f, outR = 0.0f;

        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            const int index = (int) v.position;

            if (index >= length - 1)
            {
                v.active = false;
                continue;
            }

            const float frac = (float) (v.position - index);
            const float g = v.gain * v.fade * level;
            outL += (srcL[index] + frac * (srcL[index + 1] - srcL[index])) * g;
            outR += (srcR[index] + frac * (srcR[index + 1] - srcR[index])) * g;
            v.position += v.step;

            if (v.fadeStep > 0.0f && (v.fade -= v.fadeStep) <= 0.0f)
                v.active = false;
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
}

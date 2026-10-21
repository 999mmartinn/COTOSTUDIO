#include "DrumSynthPlugin.h"

const char* DrumSynthPlugin::xmlTypeName = "drumsynth";

static const juce::Identifier soundID ("sound"), tuneID ("tune"), decayID ("decay"), levelID ("level");

juce::String DrumSynthPlugin::getSoundName (int sound)
{
    switch (sound)
    {
        case kick:  return "Kick";
        case clap:  return "Clap";
        case hat:   return "Hat";
        case snare: return "Snare";
        default:    return "Drum";
    }
}

DrumSynthPlugin::DrumSynthPlugin (te::PluginCreationInfo info) : Plugin (info)
{
    auto um = getUndoManager();
    soundValue.referTo (state, soundID, um, (int) kick);
    tuneValue.referTo  (state, tuneID,  um, 0.0f);
    decayValue.referTo (state, decayID, um, 1.0f);
    levelValue.referTo (state, levelID, um, 0.5f);

    soundParam = addParam ("sound", TRANS("Sound"), { 0.0f, (float) (numSounds - 1), 1.0f },
                           [] (float v) { return getSoundName (juce::roundToInt (v)); },
                           [] (const juce::String& s)
                           {
                               for (int i = 0; i < numSounds; ++i)
                                   if (s.equalsIgnoreCase (getSoundName (i)))
                                       return (float) i;
                               return 0.0f;
                           });
    tuneParam  = addParam ("tune",  TRANS("Tune"),  { -12.0f, 12.0f });
    decayParam = addParam ("decay", TRANS("Decay"), { 0.2f, 3.0f });
    levelParam = addParam ("level", TRANS("Level"), { 0.0f, 1.0f });

    soundParam->attachToCurrentValue (soundValue);
    tuneParam->attachToCurrentValue (tuneValue);
    decayParam->attachToCurrentValue (decayValue);
    levelParam->attachToCurrentValue (levelValue);
}

DrumSynthPlugin::~DrumSynthPlugin()
{
    notifyListenersOfDeletion();

    for (auto p : { soundParam, tuneParam, decayParam, levelParam })
        p->detachFromCurrentValue();
}

void DrumSynthPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate > 0 ? info.sampleRate : 44100.0;
    reset();
}

void DrumSynthPlugin::reset()
{
    for (auto& v : voices)
        v.active = false;
}

void DrumSynthPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, soundValue, tuneValue, decayValue, levelValue);

    for (auto p : getAutomatableParameters())
        p->updateFromAttachedValue();
}

void DrumSynthPlugin::startVoice (int noteNumber, float velocity)
{
    auto& v = voices[(size_t) nextVoice];
    nextVoice = (nextVoice + 1) % maxVoices;

    v.active = true;
    v.time = 0.0;
    v.phase = 0.0;
    v.hp = 0.0f;
    v.lastNoise = 0.0f;
    v.velocity = velocity;
    v.pitchRatio = std::pow (2.0f, (noteNumber - 60) / 12.0f);
}

float DrumSynthPlugin::renderVoiceSample (Voice& v, int sound, float tune, float decay)
{
    const double t = v.time;
    const float ratio = v.pitchRatio * std::pow (2.0f, tune / 12.0f);
    const double dt = 1.0 / sampleRate;
    float out = 0.0f;

    auto noise = [&v] { return v.random.nextFloat() * 2.0f - 1.0f; };

    // Simple one-pole high-pass on noise, for hats and snare sizzle.
    auto highPassedNoise = [&v, &noise] (float coeff)
    {
        auto n = noise();
        v.hp = coeff * (v.hp + n - v.lastNoise);
        v.lastNoise = n;
        return v.hp;
    };

    switch (sound)
    {
        case kick:
        {
            // Sine with a fast downward pitch sweep plus a short click.
            const double freq = (48.0 + 140.0 * std::exp (-t / 0.035)) * ratio;
            v.phase += juce::MathConstants<double>::twoPi * freq * dt;
            const float body = (float) std::sin (v.phase) * (float) std::exp (-t / (0.35 * decay));
            const float click = noise() * (float) std::exp (-t / 0.002) * 0.3f;
            out = body + click;
            if (t > 2.5 * decay) v.active = false;
            break;
        }

        case clap:
        {
            // Three quick noise bursts, then a short tail.
            float env = 0.0f;
            for (double start : { 0.0, 0.011, 0.022 })
                if (t >= start)
                    env = std::max (env, (float) std::exp (-(t - start) / 0.006));
            env = std::max (env, t >= 0.03 ? (float) std::exp (-(t - 0.03) / (0.12 * decay)) * 0.7f : 0.0f);
            out = highPassedNoise (0.85f) * env * 1.4f;
            if (t > 1.5 * decay) v.active = false;
            break;
        }

        case hat:
        {
            out = highPassedNoise (0.97f) * (float) std::exp (-t / (0.045 * decay)) * 0.8f;
            if (t > 0.6 * decay) v.active = false;
            break;
        }

        case snare:
        default:
        {
            const double freq = 185.0 * ratio;
            v.phase += juce::MathConstants<double>::twoPi * freq * dt;
            const float tone = (float) std::sin (v.phase) * (float) std::exp (-t / 0.07) * 0.6f;
            const float sizzle = highPassedNoise (0.9f) * (float) std::exp (-t / (0.16 * decay)) * 0.8f;
            out = tone + sizzle;
            if (t > 1.5 * decay) v.active = false;
            break;
        }
    }

    v.time += dt;
    return out * v.velocity;
}

void DrumSynthPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    auto& buffer = *fc.destBuffer;
    const int start = fc.bufferStartSample, num = fc.bufferNumSamples;
    buffer.clear (start, num);

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        reset();

    const int sound = juce::jlimit (0, numSounds - 1, juce::roundToInt (soundParam->getCurrentValue()));
    const float tune = tuneParam->getCurrentValue();
    const float decay = juce::jmax (0.05f, decayParam->getCurrentValue());
    const float level = levelParam->getCurrentValue();

    // Collect note-ons with their sample position inside this block.
    struct Hit { int pos; int note; float vel; };
    juce::Array<Hit> hits;

    if (fc.bufferForMidiMessages != nullptr)
        for (auto& m : *fc.bufferForMidiMessages)
            if (m.isNoteOn())
                hits.add ({ juce::jlimit (start, start + num - 1, juce::roundToInt (m.getTimeStamp() * sampleRate)),
                            m.getNoteNumber(), m.getFloatVelocity() });

    int hitIndex = 0;
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    for (int i = start; i < start + num; ++i)
    {
        while (hitIndex < hits.size() && hits.getReference (hitIndex).pos <= i)
        {
            auto& h = hits.getReference (hitIndex++);
            startVoice (h.note, h.vel);
        }

        float sample = 0.0f;

        for (auto& v : voices)
            if (v.active)
                sample += renderVoiceSample (v, sound, tune, decay);

        sample = std::tanh (sample * level);   // gentle safety limiter
        left[i] += sample;

        if (right != nullptr)
            right[i] += sample;
    }
}

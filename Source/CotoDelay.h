#pragma once

#include <JuceHeader.h>

/**
    The maths of COTOSTUDIO's Delay, kept apart from the engine so the self-test can
    measure it. Our own design, in the spirit of FL's Fruity Delay 3 (from its public
    manual, no FL code).

        in -> input level -> delay line(s) -> [each repeat: filter, sample rate, bits,
              diffusion, distortion] -> back into the line (feedback) and out -> tone -> wet
           -> dry

    Models: mono (one line; Offset delays one side more), stereo (one line per side;
    Offset pre-delays one side), ping pong (the sides swap on every repeat; Pan places
    the input), off (no echo: the repeat chain works as a filter / lo-fi / distortion).
*/
namespace dly
{
    enum Model { mono = 0, stereo, pingPong, off };
    enum FilterType { filterOff = 0, lowPass, highPass, bandPass };
    enum DistType { limit = 0, saturate };

    /** Tempo-synced lengths: { name, steps } (a step = a 1/16 note). */
    struct Division { const char* name; double steps; };

    inline const Division divisions[] =
    {
        { "1/64", 0.25 }, { "1/32T", 1.0 / 3.0 }, { "1/32", 0.5 }, { "1/16T", 2.0 / 3.0 }, { "1/16", 1.0 },
        { "1/8T", 4.0 / 3.0 }, { "1/16D", 1.5 }, { "1/8", 2.0 }, { "1/4T", 8.0 / 3.0 }, { "1/8D", 3.0 },
        { "1/4", 4.0 }, { "1/2T", 16.0 / 3.0 }, { "1/4D", 6.0 }, { "1/2", 8.0 }, { "1/2D", 12.0 }, { "1/1", 16.0 }
    };

    constexpr int numDivisions = (int) (sizeof (divisions) / sizeof (divisions[0]));
    constexpr double maxSeconds = 5.0;   // longest line (1 bar at 48 BPM, plus offset)

    struct Settings
    {
        int model = stereo;
        double inputLevel = 1.0;
        bool tempoSync = true;
        int division = 9;               // 1/8D
        double timeMs = 300.0;          // when not synced
        double bpm = 120.0;
        bool keepPitch = true;
        double smoothing = 0.0;         // 0..1 (how slowly a new time takes effect)
        double offset = 0.0;            // -1..1: offset (mono / stereo) or pan (ping pong)
        double spread = 1.0;            // 0 = mono wet, 1 = normal stereo

        double feedback = 0.4;          // 0..1.2
        int filterType = lowPass;
        double cutoffHz = 6000.0, resonance = 0.0;
        double sampleRateHz = 48000.0;  // at or above the real rate = off
        double bits = 24.0;             // 24 = off

        double modRateHz = 0.5, modTime = 0.0, modCutoff = 0.0;
        double diffusion = 0.0, diffusionSpread = 0.5;

        int distType = limit;
        double distKnee = 0.5, distSymmetry = 0.0, distLevelDb = 0.0;

        double wet = 0.5, tone = 0.0, dry = 1.0;

        /** The echo time in seconds. */
        double seconds() const
        {
            if (tempoSync)
                return divisions[juce::jlimit (0, numDivisions - 1, division)].steps * 0.25 * 60.0 / juce::jmax (10.0, bpm);

            return juce::jlimit (1.0, 1000.0, timeMs) * 0.001;
        }
    };

    //==============================================================================
    /** State-variable filter (topology-preserving), one channel. */
    struct Svf
    {
        double ic1 = 0, ic2 = 0;
        void reset()   { ic1 = ic2 = 0; }

        double process (double x, int type, double g, double k)
        {
            const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
            const double v3 = x - ic2;
            const double v1 = a1 * ic1 + a2 * v3;
            const double v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;

            switch (type)
            {
                case lowPass:  return v2;
                case highPass: return x - k * v1 - v2;
                case bandPass: return v1;
                default:       return x;
            }
        }
    };

    /** A short all-pass filter (for diffusion). */
    struct Allpass
    {
        std::vector<float> buf;
        int pos = 0;

        void allocate (int maxLen)    { buf.assign ((size_t) juce::jmax (2, maxLen), 0.0f); pos = 0; }
        void reset()                  { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }

        double process (double x, int len, double g)
        {
            len = juce::jlimit (1, (int) buf.size() - 1, len);
            int r = pos - len;
            if (r < 0) r += (int) buf.size();
            const double delayed = buf[(size_t) r];
            const double v = x + g * delayed;
            buf[(size_t) pos] = (float) v;
            pos = (pos + 1) % (int) buf.size();
            return delayed - g * v;
        }
    };

    /** The waveshaper used on every repeat. */
    inline double shape (double x, int type, double levelDb, double knee, double symmetry)
    {
        const double t = juce::Decibels::decibelsToGain (levelDb);

        if (type == limit)
            return juce::jlimit (-t, t, x);

        // Saturation: soft above (1 - knee) of the level; symmetry 1 = only the positive side.
        const bool negative = x < 0.0;

        if (negative && symmetry > 0.0)
        {
            // Blend between symmetric and untouched negative half.
            const double clean = x;
            const double sat = -shape (-x, saturate, levelDb, knee, 0.0);
            return clean * symmetry + sat * (1.0 - symmetry);
        }

        const double a = std::abs (x);
        const double start = t * (1.0 - juce::jlimit (0.0, 0.99, knee));

        if (a <= start)
            return x;

        const double room = juce::jmax (1.0e-9, t - start);
        return std::copysign (start + room * std::tanh ((a - start) / room), x);
    }

    //==============================================================================
    class Processor
    {
    public:
        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate > 0 ? newSampleRate : 44100.0;
            lineLength = (int) std::ceil (maxSeconds * sampleRate) + 8;
            line.setSize (2, lineLength);
            postLine.setSize (2, lineLength);

            for (auto& ap : allpasses)
                ap.allocate ((int) (0.06 * sampleRate) + 4);

            reset();
        }

        void reset()
        {
            line.clear();
            postLine.clear();
            writePos = 0;
            postPos = 0;
            preCurrent = { 0.0, 0.0 };
            lfoPhase = 0.0;
            currentTime = -1.0;
            fadeTime = -1.0;
            fadeLeft = 0;

            for (auto& f : filters) f.reset();
            for (auto& f : toneLp) f = 0.0;
            for (auto& f : toneHp) f = 0.0;
            for (auto& ap : allpasses) ap.reset();
            for (auto& h : holdValue) h = 0.0;
            holdPhase = 0.0;
        }

        void setSettings (const Settings& s)   { settings = s; }

        void process (float* const* ch, int numChannels, int numSamples)
        {
            const juce::ScopedNoDenormals noDenormals;   // decaying tails must never cost CPU
            numChannels = juce::jmin (2, numChannels);
            const auto& s = settings;
            const double target = juce::jlimit (1.0 / sampleRate, maxSeconds - 0.6, s.seconds()) * sampleRate;

            if (currentTime < 0.0)
            {
                // First block: start at the right times (no glide from zero).
                currentTime = target;
                const bool useOffset = s.model == mono || s.model == stereo;
                preCurrent = { useOffset ? target * juce::jmax (0.0, -s.offset) : 0.0,
                               useOffset ? target * juce::jmax (0.0, s.offset) : 0.0 };
            }

            // Keep pitch: a new time crossfades from the old echo to the new one.
            // Otherwise the time slides (the echo bends in pitch, like tape).
            const double slideCoef = 1.0 - std::exp (-1.0 / ((0.02 + 1.5 * s.smoothing) * sampleRate));
            const int fadeLength = (int) ((0.03 + 0.5 * s.smoothing) * sampleRate);

            const double fb = juce::jlimit (0.0, 1.2, s.feedback);
            const double res = juce::jlimit (0.0, 1.0, s.resonance);
            const double k = 2.0 - 1.95 * res;    // damping: 2 = none, 0.05 = sharp
            const double lfoStep = juce::MathConstants<double>::twoPi * s.modRateHz / sampleRate;
            const double modDepth = s.modTime * juce::jmin (0.02 * sampleRate, 0.5 * target);
            const bool lofiRate = s.sampleRateHz < sampleRate * 0.999;
            const double holdStep = s.sampleRateHz / sampleRate;
            const bool lofiBits = s.bits < 23.99;
            const double levels = std::pow (2.0, juce::jlimit (1.0, 24.0, s.bits) - 1.0);
            const double apGain = 0.7 * juce::jlimit (0.0, 1.0, s.diffusion);
            const int apBase = (int) ((0.004 + 0.03 * s.diffusionSpread) * sampleRate);
            static constexpr double apScale[4] = { 1.0, 0.77, 0.59, 0.43 };

            // Output tone: left = low pass (20 kHz down to 400 Hz), right = high pass (20 Hz up to 2 kHz).
            const double lpHz = s.tone < 0.0 ? 20000.0 * std::pow (400.0 / 20000.0, -s.tone) : 0.0;
            const double hpHz = s.tone > 0.0 ? 20.0 * std::pow (2000.0 / 20.0, s.tone) : 0.0;
            const double lpCoef = lpHz > 0.0 ? 1.0 - std::exp (-juce::MathConstants<double>::twoPi * lpHz / sampleRate) : 1.0;
            const double hpCoef = hpHz > 0.0 ? 1.0 - std::exp (-juce::MathConstants<double>::twoPi * hpHz / sampleRate) : 0.0;
            const double width = juce::jlimit (0.0, 1.0, s.spread);
            const double offset = juce::jlimit (-1.0, 1.0, s.offset);
            const double preCoef = 1.0 - std::exp (-1.0 / (0.03 * sampleRate));

            for (int i = 0; i < numSamples; ++i)
            {
                const double inL = ch[0][i];
                const double inR = numChannels > 1 ? (double) ch[1][i] : inL;

                // --- Delay time (slide or crossfade), plus modulation ---
                if (s.keepPitch)
                {
                    if (std::abs (target - currentTime) > 0.5 && fadeLeft == 0)
                    {
                        fadeTime = currentTime;   // the old echo fades out
                        currentTime = target;     // the new one fades in
                        fadeLeft = fadeLength;
                    }
                }
                else
                {
                    currentTime += (target - currentTime) * slideCoef;
                    fadeLeft = 0;
                }

                const double lfo = std::sin (lfoPhase), lfo90 = std::cos (lfoPhase);
                lfoPhase += lfoStep;
                if (lfoPhase > juce::MathConstants<double>::twoPi) lfoPhase -= juce::MathConstants<double>::twoPi;

                const double cutoff = juce::jlimit (20.0, sampleRate * 0.45,
                                                    s.cutoffHz * std::pow (2.0, s.modCutoff * 3.0 * lfo));
                const double g = std::tan (juce::MathConstants<double>::pi * cutoff / sampleRate);

                // The loop repeats at the echo time on both sides (Offset is a pre-delay on
                // the output, below); modulation moves the sides a quarter turn apart.
                auto timesFor = [&] (double base, double& tl, double& tr)
                {
                    tl = base;
                    tr = base;
                    tl = juce::jlimit (3.0, (double) lineLength - 4.0, tl + modDepth * lfo);
                    tr = juce::jlimit (3.0, (double) lineLength - 4.0, tr + modDepth * lfo90);
                };

                double tl, tr;
                timesFor (currentTime, tl, tr);
                double echo[2] = { read (0, tl), read (s.model == mono ? 0 : 1, tr) };

                if (fadeLeft > 0)
                {
                    double ol, orr;
                    timesFor (fadeTime, ol, orr);
                    const double old[2] = { read (0, ol), read (s.model == mono ? 0 : 1, orr) };
                    const double a = (double) fadeLeft / (double) juce::jmax (1, fadeLength);   // 1 -> 0
                    echo[0] = old[0] * a + echo[0] * (1.0 - a);
                    echo[1] = old[1] * a + echo[1] * (1.0 - a);
                    --fadeLeft;
                }

                if (s.model == off)
                {
                    echo[0] = inL;   // no echo: the chain processes the input itself
                    echo[1] = inR;
                }

                // --- The repeat chain: filter, sample rate, bits, diffusion, distortion ---
                double y[2];

                for (int c = 0; c < 2; ++c)
                    y[c] = filters[(size_t) c].process (echo[c], s.filterType, g, k);

                if (lofiRate)
                {
                    // Sample and hold at the lower rate (aliasing included, like the real thing).
                    holdPhase += holdStep;

                    if (holdPhase >= 1.0)
                    {
                        holdPhase -= 1.0;
                        holdValue[0] = y[0];
                        holdValue[1] = y[1];
                    }

                    y[0] = holdValue[0];
                    y[1] = holdValue[1];
                }

                for (int c = 0; c < 2; ++c)
                {
                    double v = y[c];

                    if (lofiBits)  v = std::round (v * levels) / levels;

                    if (apGain > 0.0)
                        for (int a = 0; a < 2; ++a)
                            v = allpasses[(size_t) (c * 2 + a)].process (v, (int) (apBase * apScale[(size_t) (c * 2 + a)]), apGain);

                    y[c] = shape (v, s.distType, s.distLevelDb, s.distKnee, s.distSymmetry);
                }

                // --- Back into the line ---
                if (s.model != off)
                {
                    const double inLevel = s.inputLevel;
                    double wl, wr;

                    if (s.model == mono)
                    {
                        wl = 0.5 * (inL + inR) * inLevel + fb * y[0];
                        wr = 0.0;
                    }
                    else if (s.model == stereo)
                    {
                        wl = inL * inLevel + fb * y[0];
                        wr = inR * inLevel + fb * y[1];
                    }
                    else   // ping pong: the sides swap on every repeat; Pan places the input
                    {
                        const double gl = juce::jmin (1.0, 1.0 - offset), gr = juce::jmin (1.0, 1.0 + offset);
                        wl = inL * inLevel * gl + fb * y[1];
                        wr = inR * inLevel * gr + fb * y[0];
                    }

                    line.setSample (0, writePos, (float) wl);
                    line.setSample (1, writePos, (float) wr);
                    writePos = (writePos + 1) % lineLength;
                }

                // --- Output: tone, stereo spread, wet / dry ---
                double out[2];

                for (int c = 0; c < 2; ++c)
                {
                    double v = y[c];
                    toneLp[(size_t) c] += (v - toneLp[(size_t) c]) * lpCoef;
                    v = toneLp[(size_t) c];

                    if (hpCoef > 0.0)
                    {
                        toneHp[(size_t) c] += (v - toneHp[(size_t) c]) * hpCoef;
                        v -= toneHp[(size_t) c];
                    }

                    out[c] = v;
                }

                // Offset (mono / stereo): one side's echoes come later, by a share of the time.
                const bool useOffset = s.model == mono || s.model == stereo;
                const double preTarget[2] = { useOffset ? currentTime * juce::jmax (0.0, -offset) : 0.0,
                                              useOffset ? currentTime * juce::jmax (0.0, offset) : 0.0 };

                for (int c = 0; c < 2; ++c)
                {
                    preCurrent[(size_t) c] += (preTarget[c] - preCurrent[(size_t) c]) * preCoef;
                    postLine.setSample (c, postPos, (float) out[c]);
                    out[c] = readLinear (postLine, c, postPos, preCurrent[(size_t) c]);
                }

                postPos = (postPos + 1) % lineLength;

                const double mid = 0.5 * (out[0] + out[1]), side = 0.5 * (out[0] - out[1]) * width;
                const double dryL = inL * s.dry, dryR = inR * s.dry;
                ch[0][i] = (float) (dryL + (mid + side) * s.wet);

                if (numChannels > 1)
                    ch[1][i] = (float) (dryR + (mid - side) * s.wet);
            }
        }

    private:
        Settings settings;
        double sampleRate = 44100.0;
        juce::AudioBuffer<float> line, postLine;
        int lineLength = 1, writePos = 0, postPos = 0;
        std::array<double, 2> preCurrent {};
        double currentTime = -1.0, fadeTime = -1.0;
        int fadeLeft = 0;
        double lfoPhase = 0.0, holdPhase = 0.0;
        std::array<Svf, 2> filters;
        std::array<double, 2> toneLp {}, toneHp {}, holdValue {};
        std::array<Allpass, 4> allpasses;

        /** Reads `delay` samples before `at` (0 = the sample just written), linear. */
        double readLinear (const juce::AudioBuffer<float>& buf, int c, int at, double delay) const
        {
            double pos = (double) at - juce::jlimit (0.0, (double) lineLength - 2.0, delay);
            while (pos < 0.0) pos += lineLength;
            const int i0 = (int) pos;
            const double f = pos - i0;
            const double a = buf.getSample (c, i0 % lineLength);
            return f > 0.0 ? a + (buf.getSample (c, (i0 + 1) % lineLength) - a) * f : a;
        }

        /** Reads `delay` samples back (fractional: 4-point Hermite). */
        double read (int c, double delay) const
        {
            double pos = (double) writePos - delay;
            while (pos < 0.0) pos += lineLength;

            const int i1 = (int) pos;
            const double f = pos - i1;
            auto at = [this, c] (int i) { i %= lineLength; if (i < 0) i += lineLength; return (double) line.getSample (c, i); };
            const double y0 = at (i1 - 1), y1 = at (i1), y2 = at (i1 + 1), y3 = at (i1 + 2);
            const double c1 = 0.5 * (y2 - y0);
            const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
            const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
            return ((c3 * f + c2) * f + c1) * f + y1;
        }
    };
}

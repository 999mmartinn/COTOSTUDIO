#pragma once

#include <JuceHeader.h>
#include "CotoDelay.h"   // the tempo divisions (pre-delay in note values)

/**
    The maths of COTOSTUDIO's Reverb, kept apart from the engine so the self-test can
    measure it. Our own design, in the spirit of FL's Fruity Reeverb 2 (from its public
    manual, no FL code):

        in -> mid (or side) -> pre-delay -> low / high cut -> early reflections
                                                         -> diffusion (4 all-passes)
                                                         -> 8-line feedback delay network
                                                            (decay per band, damping, modulation)
        out = dry + early reflections + late reverb (stereo separation)

    The decay knob is the RT60: the time the tail takes to fall by 60 dB. The bass
    multiplier makes frequencies under the crossover last that many times longer.
*/
namespace rvb
{
    struct Settings
    {
        double lowCutHz = 70.0, highCutHz = 12000.0;
        bool tempoPredelay = false;
        double predelayMs = 20.0;
        int predelayDivision = 0;
        double bpm = 120.0;
        double size = 0.6, diffusion = 0.7;
        double modDepth = 0.2, modSpeedHz = 0.6;
        double decaySeconds = 2.5, bassMultiplier = 1.0, crossoverHz = 300.0;
        double dampHz = 8000.0;           // 20 kHz or more = off
        double dry = 1.0, early = 0.4, wet = 0.25, separation = 1.0;
        bool side = false;                // process the side (L - R) instead of the mid

        double predelaySeconds() const
        {
            if (tempoPredelay)
                return dly::divisions[juce::jlimit (0, dly::numDivisions - 1, predelayDivision)].steps * 0.25 * 60.0 / juce::jmax (10.0, bpm);

            return juce::jlimit (0.0, 500.0, predelayMs) * 0.001;
        }
    };

    constexpr int numLines = 8;
    constexpr double maxPredelaySeconds = 2.5;

    // Line lengths (ms at the largest room), and early reflection taps (ms, gain).
    inline constexpr double lineMs[numLines] = { 59.1, 67.3, 73.9, 81.7, 89.3, 97.1, 103.9, 113.3 };
    inline constexpr double diffuserMs[4] = { 4.71, 3.63, 12.73, 9.31 };
    struct Tap { double ms, gain; };
    inline constexpr Tap tapsL[8] = { { 7.1, 0.82 }, { 11.3, -0.66 }, { 17.9, 0.58 }, { 23.3, -0.47 }, { 31.7, 0.39 }, { 41.9, -0.31 }, { 53.1, 0.25 }, { 67.7, -0.19 } };
    inline constexpr Tap tapsR[8] = { { 8.3, 0.79 }, { 13.1, -0.63 }, { 19.7, 0.55 }, { 26.9, -0.45 }, { 35.3, 0.37 }, { 44.3, -0.29 }, { 57.7, 0.23 }, { 71.3, -0.18 } };

    /** Room size 0..1 -> how much the delay lengths are scaled. */
    inline double sizeScale (double size)   { return 0.25 + 0.75 * juce::jlimit (0.0, 1.0, size); }

    /** One-pole filter helpers. */
    inline double onePoleCoef (double hz, double sampleRate)
    {
        return 1.0 - std::exp (-juce::MathConstants<double>::twoPi * juce::jlimit (1.0, sampleRate * 0.49, hz) / sampleRate);
    }

    //==============================================================================
    class Processor
    {
    public:
        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate > 0 ? newSampleRate : 44100.0;
            const double maxModSamples = 0.008 * sampleRate;   // modulation reaches 2 x 3 ms

            predelay.assign ((size_t) (maxPredelaySeconds * sampleRate) + 4, 0.0f);
            erLine.assign ((size_t) (0.08 * sampleRate) + 4, 0.0f);

            for (int i = 0; i < numLines; ++i)
                lines[(size_t) i].assign ((size_t) (lineMs[i] * 0.001 * sampleRate + maxModSamples) + 8, 0.0f);

            for (int i = 0; i < 4; ++i)
                diffusers[(size_t) i].assign ((size_t) (diffuserMs[i] * 0.001 * sampleRate) + 4, 0.0f);

            reset();
        }

        void reset()
        {
            std::fill (predelay.begin(), predelay.end(), 0.0f);
            std::fill (erLine.begin(), erLine.end(), 0.0f);
            for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
            for (auto& d : diffusers) std::fill (d.begin(), d.end(), 0.0f);
            predelayPos = erPos = 0;
            linePos.fill (0);
            diffPos.fill (0);
            lowState.fill (0.0);
            dampState.fill (0.0);
            hpState = lpState = 0.0;
            lfoPhase = 0.0;
        }

        void setSettings (const Settings& s)   { settings = s; }

        void process (float* const* ch, int numChannels, int numSamples)
        {
            const juce::ScopedNoDenormals noDenormals;   // decaying tails must never cost CPU
            numChannels = juce::jmin (2, numChannels);
            const auto& s = settings;
            const double scale = sizeScale (s.size);
            const int pre = juce::jlimit (0, (int) predelay.size() - 2, (int) (s.predelaySeconds() * sampleRate));
            const double hpCoef = onePoleCoef (s.lowCutHz, sampleRate);
            const double lpCoef = s.highCutHz >= 19999.0 ? 1.0 : onePoleCoef (s.highCutHz, sampleRate);
            const double crossCoef = onePoleCoef (s.crossoverHz, sampleRate);
            const bool damping = s.dampHz < 19999.0;
            const double dampCoef = damping ? onePoleCoef (s.dampHz, sampleRate) : 1.0;
            const double apGain = 0.75 * juce::jlimit (0.0, 1.0, s.diffusion);
            const double modSamples = juce::jlimit (0.0, 1.0, s.modDepth) * 0.003 * sampleRate;
            const double lfoStep = juce::MathConstants<double>::twoPi * juce::jlimit (0.01, 10.0, s.modSpeedHz) / sampleRate;
            const double rt = juce::jlimit (0.05, 60.0, s.decaySeconds);
            const double rtLow = juce::jlimit (0.05, 120.0, rt * juce::jlimit (0.1, 8.0, s.bassMultiplier));

            // Per-line lengths and decay gains for this block.
            std::array<double, numLines> length, gHigh, gLow;

            for (int i = 0; i < numLines; ++i)
            {
                // Whole samples: with no modulation the reads are exact (any interpolation on
                // every trip round the loop would dull the tail and shorten the decay).
                length[(size_t) i] = std::round (lineMs[i] * 0.001 * sampleRate * scale);
                const double seconds = (length[(size_t) i] + modSamples) / sampleRate;   // the average modulated length
                gHigh[(size_t) i] = std::pow (10.0, -3.0 * seconds / rt);      // -60 dB after rt seconds
                gLow[(size_t) i]  = std::pow (10.0, -3.0 * seconds / rtLow);
            }

            const int diffLen[4] = { (int) (diffuserMs[0] * 0.001 * sampleRate * scale) + 1, (int) (diffuserMs[1] * 0.001 * sampleRate * scale) + 1,
                                     (int) (diffuserMs[2] * 0.001 * sampleRate * scale) + 1, (int) (diffuserMs[3] * 0.001 * sampleRate * scale) + 1 };
            const double outGain = 1.0 / std::sqrt ((double) numLines / 2.0);

            for (int n = 0; n < numSamples; ++n)
            {
                const double inL = ch[0][n];
                const double inR = numChannels > 1 ? (double) ch[1][n] : inL;
                const double source = s.side ? 0.5 * (inL - inR) : 0.5 * (inL + inR);

                // Pre-delay
                predelay[(size_t) predelayPos] = (float) source;
                int r = predelayPos - pre;
                if (r < 0) r += (int) predelay.size();
                double x = predelay[(size_t) r];
                predelayPos = (predelayPos + 1) % (int) predelay.size();

                // Low cut, high cut (on what goes into the reverb)
                hpState += (x - hpState) * hpCoef;
                x -= hpState;
                lpState += (x - lpState) * lpCoef;
                x = lpState;

                // Early reflections (taps scaled by the room size)
                erLine[(size_t) erPos] = (float) x;
                double erL = 0.0, erR = 0.0;

                for (int t = 0; t < 8; ++t)
                {
                    erL += tapsL[t].gain * readInt (erLine, erPos, (int) (tapsL[t].ms * 0.001 * sampleRate * scale));
                    erR += tapsR[t].gain * readInt (erLine, erPos, (int) (tapsR[t].ms * 0.001 * sampleRate * scale));
                }

                erPos = (erPos + 1) % (int) erLine.size();

                // Diffusion
                double d = x;

                for (int a = 0; a < 4; ++a)
                {
                    auto& buf = diffusers[(size_t) a];
                    auto& pos = diffPos[(size_t) a];
                    const double delayed = readInt (buf, pos, diffLen[a]);
                    const double v = d + apGain * delayed;
                    buf[(size_t) pos] = (float) v;
                    pos = (pos + 1) % (int) buf.size();
                    d = delayed - apGain * v;
                }

                // Late reverb: read the lines (modulated), filter, mix (Householder), write back.
                std::array<double, numLines> y;
                const double lfo = lfoPhase;
                lfoPhase += lfoStep;
                if (lfoPhase > juce::MathConstants<double>::twoPi) lfoPhase -= juce::MathConstants<double>::twoPi;

                double sum = 0.0;

                for (int i = 0; i < numLines; ++i)
                {
                    const double mod = modSamples * std::sin (lfo + i * 0.785);
                    double v = readFrac (lines[(size_t) i], linePos[(size_t) i], length[(size_t) i] + modSamples + mod);

                    // Two bands: under the crossover decays by gLow, above by gHigh.
                    lowState[(size_t) i] += (v - lowState[(size_t) i]) * crossCoef;
                    const double low = lowState[(size_t) i];
                    v = low * gLow[(size_t) i] + (v - low) * gHigh[(size_t) i];

                    if (damping)
                    {
                        dampState[(size_t) i] += (v - dampState[(size_t) i]) * dampCoef;
                        v = dampState[(size_t) i];
                    }

                    y[(size_t) i] = v;
                    sum += v;
                }

                const double h = 2.0 / numLines * sum;   // Householder reflection: energy preserving

                for (int i = 0; i < numLines; ++i)
                {
                    auto& buf = lines[(size_t) i];
                    auto& pos = linePos[(size_t) i];
                    const double fbv = y[(size_t) i] - h;
                    // The diffused input goes into every line with alternating signs.
                    buf[(size_t) pos] = (float) (fbv + ((i & 1) ? -d : d) * 0.5);
                    pos = (pos + 1) % (int) buf.size();
                }

                // Outputs: four lines to each side.
                double lateL = (y[0] - y[2] + y[4] - y[6]) * outGain;
                double lateR = (y[1] - y[3] + y[5] - y[7]) * outGain;

                // Stereo separation of the wet sound (0 = mono, 1 = normal).
                const double mid = 0.5 * (lateL + lateR), sideW = 0.5 * (lateL - lateR) * s.separation;
                lateL = mid + sideW;
                lateR = mid - sideW;
                const double erMid = 0.5 * (erL + erR), erSide = 0.5 * (erL - erR) * s.separation;

                double outL = inL * s.dry + (erMid + erSide) * s.early * 0.5 + lateL * s.wet;
                double outR = inR * s.dry + (erMid - erSide) * s.early * 0.5 + lateR * s.wet;

                if (s.side)
                {
                    // A side reverb belongs to the sides: keep it out of phase.
                    const double wl = outL - inL * s.dry, wr = outR - inR * s.dry;
                    const double sideOnly = 0.5 * (wl - wr);
                    outL = inL * s.dry + sideOnly;
                    outR = inR * s.dry - sideOnly;
                }

                ch[0][n] = (float) outL;

                if (numChannels > 1)
                    ch[1][n] = (float) outR;
            }

            // Flush denormals in the feedback states.
            for (auto& v : lowState)  if (std::abs (v) < 1.0e-20) v = 0.0;
            for (auto& v : dampState) if (std::abs (v) < 1.0e-20) v = 0.0;
        }

    private:
        Settings settings;
        double sampleRate = 44100.0;
        std::vector<float> predelay, erLine;
        std::array<std::vector<float>, numLines> lines;
        std::array<std::vector<float>, 4> diffusers;
        int predelayPos = 0, erPos = 0;
        std::array<int, numLines> linePos {};
        std::array<int, 4> diffPos {};
        std::array<double, numLines> lowState {}, dampState {};
        double hpState = 0.0, lpState = 0.0, lfoPhase = 0.0;

        static double readInt (const std::vector<float>& buf, int writePos, int delay)
        {
            const int size = (int) buf.size();
            int r = writePos - juce::jlimit (0, size - 1, delay);
            if (r < 0) r += size;
            return buf[(size_t) r];
        }

        /** Fractional read (4-point Hermite: flat up to the highs, so the tail keeps its air). */
        static double readFrac (const std::vector<float>& buf, int writePos, double delay)
        {
            const int size = (int) buf.size();
            delay = juce::jlimit (2.0, (double) size - 3.0, delay);
            double p = (double) writePos - delay;
            while (p < 0.0) p += size;
            const int i1 = (int) p;
            const double f = p - i1;

            if (f < 1.0e-9)
                return buf[(size_t) (i1 % size)];

            auto at = [&buf, size] (int i) { i %= size; if (i < 0) i += size; return (double) buf[(size_t) i]; };
            const double y0 = at (i1 - 1), y1 = at (i1), y2 = at (i1 + 1), y3 = at (i1 + 2);
            const double c1 = 0.5 * (y2 - y0);
            const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
            const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
            return ((c3 * f + c2) * f + c1) * f + y1;
        }
    };
}

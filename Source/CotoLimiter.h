#pragma once

#include <JuceHeader.h>

/**
    The maths of COTOSTUDIO's Limiter, kept apart from the engine so the self-test can
    measure it. Our own design, in the spirit of FL's Fruity Limiter (from its public
    manual, no FL code). Signal flow:

        in -> compressor (RMS) -> gain -> limiter (peak, look-ahead) + noise gate -> saturation -> out

    The limiter's look-ahead equals its attack time: the audio is delayed by it so the
    gain can come down before a peak arrives, and the output never goes over the ceiling.
*/
namespace lim
{
    struct Settings
    {
        // Compressor (threshold 0 dB = off)
        double compGainDb = 0.0, compThreshDb = 0.0, compRatio = 4.0, compKneeDb = 0.0;
        double compAttackMs = 2.0, compReleaseMs = 100.0, compSustainMs = 0.0;
        int compCurve = 2;

        // Limiter (ceiling +12 dB = effectively off)
        double limGainDb = 0.0, ceilingDb = 0.0, limReleaseMs = 80.0, limSustainMs = 0.0;
        int limCurve = 2;

        // Noise gate (gain 1 = no gating)
        double gateGain = 1.0, gateThreshDb = -60.0, gateReleaseMs = 100.0;

        // Saturation: soft clipping starts this many dB below the ceiling (0 = off)
        double satDb = 0.0;
    };

    /** One frame for the editor's display (every ~5 ms). */
    struct Frame
    {
        float inPeak = 0.0f, outPeak = 0.0f, inRms = 0.0f, gainDb = 0.0f;
    };

    inline double coefFor (double ms, double sampleRate)
    {
        return ms <= 0.0 ? 1.0 : 1.0 - std::exp (-1.0 / (ms * 0.001 * sampleRate));
    }

    /** A smoother made of `stages` one-pole filters (1 = immediate shape, 8 = slow S-curve).
        The total time stays about the same whatever the number of stages. */
    struct CurveSmoother
    {
        std::array<double, 8> z {};
        void reset (double v)    { z.fill (v); }

        double process (double target, double coefAttack, double coefRelease, int stages, bool falling)
        {
            stages = juce::jlimit (1, 8, stages);
            const double c = falling ? coefAttack : coefRelease;
            const double perStage = 1.0 - std::pow (1.0 - c, (double) stages);   // keeps the total time similar
            double v = target;

            for (int s = 0; s < stages; ++s)
            {
                z[(size_t) s] += (v - z[(size_t) s]) * perStage;
                v = z[(size_t) s];
            }

            return v;
        }
    };

    /** Fixed-size helpers (no memory is allocated while audio runs). */
    struct MinQueue   // the minimum of the last N values, O(1) per sample
    {
        std::vector<juce::int64> index;
        std::vector<double> value;
        int head = 0, count = 0;

        void allocate (int capacity)   { index.assign ((size_t) capacity, 0); value.assign ((size_t) capacity, 0.0); clear(); }
        void clear()                   { head = 0; count = 0; }
        int cap() const                { return (int) value.size(); }
        int at (int k) const           { return (head + k) % cap(); }

        double push (juce::int64 i, double v, int window)
        {
            while (count > 0 && value[(size_t) at (count - 1)] >= v)
                --count;

            index[(size_t) at (count)] = i;
            value[(size_t) at (count)] = v;
            ++count;

            while (index[(size_t) head] < i - window)
            {
                head = (head + 1) % cap();
                --count;
            }

            return value[(size_t) head];
        }
    };

    struct MovingAverage   // the mean of the last N values
    {
        std::vector<double> ring;
        int pos = 0, filled = 0;
        double sum = 0.0;

        void allocate (int capacity)   { ring.assign ((size_t) capacity, 0.0); clear(); }
        void clear()                   { pos = 0; filled = 0; sum = 0.0; }

        double push (double v, int length, juce::int64 sampleIndex)
        {
            length = juce::jlimit (1, (int) ring.size(), length);

            if (filled >= length)
            {
                int oldest = pos - length;
                if (oldest < 0) oldest += (int) ring.size();
                sum -= ring[(size_t) oldest];
            }
            else
            {
                ++filled;
            }

            ring[(size_t) pos] = v;
            sum += v;
            pos = (pos + 1) % (int) ring.size();

            if ((sampleIndex & 4095) == 0)   // stop rounding errors building up
            {
                sum = 0.0;
                for (int k = 1; k <= filled; ++k)
                    sum += ring[(size_t) ((pos - k + (int) ring.size()) % (int) ring.size())];
            }

            return sum / filled;
        }
    };

    //==============================================================================
    class Processor
    {
    public:
        static constexpr double maxLookaheadMs = 10.0;

        void prepare (double newSampleRate, int maxBlock)
        {
            sampleRate = newSampleRate > 0 ? newSampleRate : 44100.0;
            const int maxL = (int) std::ceil (maxLookaheadMs * 0.001 * sampleRate) + 1;
            delayLine.setSize (2, maxL + 4);
            minQueue.allocate (maxL + 4);
            average.allocate (maxL + 4);
            juce::ignoreUnused (maxBlock);
            reset();
        }

        void reset()
        {
            delayLine.clear();
            writePos = 0;
            compSmooth.reset (0.0);
            limSmooth.reset (1.0);
            meanSquare = 0.0;
            gate = 1.0;
            holdLeft = 0;
            limGain = 1.0;
            minQueue.clear();
            average.clear();
            sampleIndex = 0;
            frameCount = 0;
            frame = {};
        }

        /** Look-ahead in samples (set only when the engine knows the new latency). */
        void setLookaheadSamples (int n)
        {
            n = juce::jlimit (0, delayLine.getNumSamples() - 2, n);

            if (n != lookahead)
            {
                lookahead = n;
                minQueue.clear();
                average.clear();
            }
        }

        int getLookaheadSamples() const      { return lookahead; }

        /** How many samples the final safety clamp had to catch (should stay 0). */
        juce::int64 safetyClamps = 0;
        void setSettings (const Settings& s) { settings = s; }

        /** Display frames come out here (message thread reads them). */
        std::function<void (const Frame&)> onFrame;

        void process (float* const* ch, int numChannels, int numSamples)
        {
            const juce::ScopedNoDenormals noDenormals;   // decaying tails must never cost CPU
            numChannels = juce::jmin (2, numChannels);
            const auto& s = settings;

            const bool compOn = s.compThreshDb < -0.01;
            const double compMakeup = juce::Decibels::decibelsToGain (s.compGainDb);
            const double limIn = juce::Decibels::decibelsToGain (s.limGainDb);
            const double ceiling = juce::Decibels::decibelsToGain (s.ceilingDb);
            const double cAtt = coefFor (s.compAttackMs, sampleRate), cRel = coefFor (s.compReleaseMs, sampleRate);
            const double cRms = coefFor (juce::jmax (0.1, s.compSustainMs), sampleRate);
            const double lRel = coefFor (s.limReleaseMs, sampleRate);
            const double gRel = coefFor (s.gateReleaseMs, sampleRate);
            const double gateThresh = juce::Decibels::decibelsToGain (s.gateThreshDb);
            const int holdSamples = (int) (s.limSustainMs * 0.001 * sampleRate);
            const double satStart = ceiling * juce::Decibels::decibelsToGain (-juce::jmax (0.0, s.satDb));
            const bool satOn = s.satDb > 0.01;
            const int L = lookahead;
            const int dlSize = delayLine.getNumSamples();
            const int frameLength = juce::jmax (1, (int) (sampleRate * 0.005));

            for (int i = 0; i < numSamples; ++i)
            {
                double x[2] = { ch[0][i], numChannels > 1 ? (double) ch[1][i] : (double) ch[0][i] };
                const double inPeak = juce::jmax (std::abs (x[0]), std::abs (x[1]));
                frame.inPeak = juce::jmax (frame.inPeak, (float) inPeak);

                // --- Compressor (RMS level, soft knee) ---
                const double ms = 0.5 * (x[0] * x[0] + x[1] * x[1]);
                meanSquare += (ms - meanSquare) * (s.compSustainMs > 0.0 ? cRms : 1.0);
                const double rmsLevel = std::sqrt (juce::jmax (0.0, meanSquare));
                frame.inRms = juce::jmax (frame.inRms, (float) rmsLevel);
                double compGain = 1.0;

                if (compOn)
                {
                    const double levelDb = juce::Decibels::gainToDecibels (s.compSustainMs > 0.0 ? rmsLevel : inPeak, -200.0);
                    const double over = levelDb - s.compThreshDb;
                    const double knee = juce::jmax (0.0, s.compKneeDb);
                    const double slope = 1.0 - 1.0 / juce::jmax (1.0, s.compRatio);
                    double grDb = 0.0;

                    if (knee > 0.0 && std::abs (over) <= knee * 0.5)
                        grDb = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
                    else if (over > 0.0)
                        grDb = slope * over;

                    const double smoothed = compSmooth.process (grDb, cAtt, cRel, s.compCurve, grDb > compSmooth.z[0]);
                    compGain = juce::Decibels::decibelsToGain (-smoothed);
                }

                const double pre = compGain * compMakeup * limIn;
                x[0] *= pre;
                x[1] *= pre;

                // --- Noise gate (opens at once, closes over its release) ---
                const double gateLevel = juce::jmax (std::abs (x[0]), std::abs (x[1]));

                if (s.gateGain < 0.999)
                    gate = gateLevel >= gateThresh ? 1.0 : gate + (s.gateGain - gate) * gRel;
                else
                    gate = 1.0;

                // --- Limiter: the gain this sample needs, then look-ahead smoothing ---
                const double peak = juce::jmax (std::abs (x[0]), std::abs (x[1])) * gate;
                const double needed = peak > ceiling ? ceiling / peak : 1.0;

                // Minimum of the last L+1 needed gains, then their average over L+1 samples:
                // a ramp of L samples into each peak that never lets one through.
                const double held = minQueue.push (sampleIndex, needed, L);
                const double ramp = juce::jmin (1.0, average.push (held, L + 1, sampleIndex));

                // Release: never above what is needed; rises slowly after a hold.
                if (ramp < limGain)
                {
                    limGain = ramp;
                    holdLeft = holdSamples;
                    limSmooth.reset (limGain);
                }
                else if (holdLeft > 0)
                {
                    --holdLeft;
                }
                else
                {
                    limGain = juce::jmin (ramp, limSmooth.process (ramp, 1.0, lRel, s.limCurve, false));
                }

                // --- Delay the audio by the look-ahead, then apply ---
                for (int c = 0; c < 2; ++c)
                    delayLine.setSample (c, writePos, (float) (x[c] * gate));

                int readPos = writePos - L;
                if (readPos < 0) readPos += dlSize;
                writePos = (writePos + 1) % dlSize;

                double out[2];

                for (int c = 0; c < 2; ++c)
                {
                    double y = delayLine.getSample (c, readPos) * limGain;

                    if (satOn)
                    {
                        // Soft clip between satStart and the ceiling.
                        const double a = std::abs (y);

                        if (a > satStart)
                        {
                            const double room = juce::jmax (1.0e-9, ceiling - satStart);
                            y = std::copysign (satStart + room * std::tanh ((a - satStart) / room), y);
                        }
                    }

                    // Safety: nothing ever leaves above the ceiling (rounding only; counted
                    // so the self-test can prove the look-ahead did the work, not this).
                    if (std::abs (y) > ceiling * (1.0 + 1.0e-6))
                        ++safetyClamps;

                    out[c] = juce::jlimit (-ceiling, ceiling, y);
                }

                ch[0][i] = (float) out[0];

                if (numChannels > 1)
                    ch[1][i] = (float) out[1];

                frame.outPeak = juce::jmax (frame.outPeak, (float) juce::jmax (std::abs (out[0]), std::abs (out[1])));
                frame.gainDb = juce::jmin (frame.gainDb, (float) juce::Decibels::gainToDecibels (limGain * compGain * gate, -60.0));
                ++sampleIndex;

                if (++frameCount >= frameLength)
                {
                    if (onFrame)
                        onFrame (frame);

                    frame = {};
                    frameCount = 0;
                }
            }
        }

    private:
        Settings settings;
        double sampleRate = 44100.0;
        juce::AudioBuffer<float> delayLine;
        int writePos = 0, lookahead = 0;
        CurveSmoother compSmooth, limSmooth;
        double meanSquare = 0.0, gate = 1.0, limGain = 1.0;
        int holdLeft = 0, frameCount = 0;
        juce::int64 sampleIndex = 0;
        MinQueue minQueue;
        MovingAverage average;
        Frame frame;
    };
}

#pragma once

#include <JuceHeader.h>

/**
    Listening to audio without playing it (2.4):

      - findSoundRegions: where a recording has sound and where it is quiet, for the
        playlist's "Strip silence" (cuts the quiet parts out of a take).
      - estimateBpm: a loop's tempo, from how regularly its hits come.
      - estimateKey: a sound's key (C minor, F# major...), from which notes are in it.

    All three work on plain numbers (no files, no engine), so the self-test can check
    them with made-up signals.
*/
namespace audiotools
{
    /** The parts with sound. `windowPeaks` holds the loudest sample (0..1) of each
        `windowSeconds` of audio. Quiet = under `thresholdDb`; a quiet gap shorter than
        `minSilence` doesn't split a part; each part gets a little room before and after
        (so the first consonant and the tail of a word stay). Seconds from the start. */
    inline std::vector<juce::Range<double>> findSoundRegions (const std::vector<float>& windowPeaks, double windowSeconds,
                                                              float thresholdDb, double minSilence = 0.3,
                                                              double roomBefore = 0.04, double roomAfter = 0.15)
    {
        std::vector<juce::Range<double>> regions;
        const float threshold = juce::Decibels::decibelsToGain (thresholdDb);
        const double total = (double) windowPeaks.size() * windowSeconds;
        int start = -1;

        auto finish = [&] (int endWindow)
        {
            const double from = juce::jmax (0.0, start * windowSeconds - roomBefore);
            const double to = juce::jmin (total, endWindow * windowSeconds + roomAfter);

            // (Runs that touch or overlap once they have their room become one.)
            if (! regions.empty() && from - regions.back().getEnd() < minSilence)
                regions.back().setEnd (juce::jmax (regions.back().getEnd(), to));
            else
                regions.push_back ({ from, to });

            start = -1;
        };

        for (int i = 0; i < (int) windowPeaks.size(); ++i)
        {
            const bool loud = windowPeaks[(size_t) i] >= threshold;

            if (loud && start < 0)       start = i;
            else if (! loud && start >= 0) finish (i);
        }

        if (start >= 0)
            finish ((int) windowPeaks.size());

        return regions;
    }

    /** The threshold "Strip silence" uses for a take whose loudest sample is `peak`:
        40 dB under it, but never above -36 dB or below -60 dB. */
    inline float silenceThresholdDb (float peak)
    {
        return juce::jlimit (-60.0f, -36.0f, juce::Decibels::gainToDecibels (peak, -100.0f) - 40.0f);
    }

    //==============================================================================
    /** A loop's tempo in BPM (70..180; between a tempo and its double, the one nearer
        120), or 0 when no steady beat is found (a single hit, a voice, a pad). Mono samples;
        only the first 30 s are looked at. */
    inline double estimateBpm (const float* mono, int numSamples, double sampleRate)
    {
        if (mono == nullptr || sampleRate <= 0.0)
            return 0.0;

        numSamples = juce::jmin (numSamples, (int) (sampleRate * 30.0));
        const int hop = juce::jmax (1, juce::roundToInt (sampleRate * 0.005));
        const int n = numSamples / hop;

        if (n < 400)   // under 2 s
            return 0.0;

        // How much louder each 5 ms is than the one before (hits stand out).
        std::vector<double> rise ((size_t) n, 0.0);
        double before = 0.0, mean = 0.0;

        for (int i = 0; i < n; ++i)
        {
            double energy = 0.0;

            for (int j = i * hop; j < (i + 1) * hop; ++j)
                energy += (double) mono[j] * mono[j];

            const double level = std::log10 (1.0e-9 + energy / hop);
            rise[(size_t) i] = i == 0 ? 0.0 : juce::jmax (0.0, level - before);
            before = level;
            mean += rise[(size_t) i];
        }

        mean /= n;

        for (auto& r : rise)
            r -= mean;

        // How well the hits line up with themselves a while later.
        auto match = [&] (int lag)
        {
            if (lag >= n)
                return 0.0;

            double sum = 0.0;

            for (int i = 0; i < n - lag; ++i)
                sum += rise[(size_t) i] * rise[(size_t) (i + lag)];

            return sum / (n - lag);
        };

        const double strength = match (0);

        if (strength < 3.0e-3)
            return 0.0;

        const double rate = sampleRate / hop;
        const int shortest = (int) (rate * 60.0 / 180.0), longest = (int) (rate * 60.0 / 70.0) + 1;
        std::vector<double> scores ((size_t) (longest - shortest + 3), 0.0);
        auto score = [&] (int lag) -> double&    { return scores[(size_t) (lag - shortest + 1)]; };

        for (int lag = shortest - 1; lag <= longest + 1; ++lag)
            score (lag) = match (lag) + 0.5 * match (2 * lag) + 0.25 * match (4 * lag);

        int best = shortest;
        double bestScore = -1.0e9;

        for (int lag = shortest; lag <= longest; ++lag)
        {
            const double octaves = std::log2 (60.0 * rate / lag / 120.0) / 0.8;
            const double weighted = score (lag) * std::exp (-0.5 * octaves * octaves);

            if (weighted > bestScore)
            {
                bestScore = weighted;
                best = lag;
            }
        }

        if (match (best) / strength < 0.2)
            return 0.0;

        // Between the lags on either side, where the peak really is.
        const double y0 = score (best - 1), y1 = score (best), y2 = score (best + 1);
        const double bend = y0 - 2.0 * y1 + y2;
        const double offset = std::abs (bend) > 1.0e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / bend) : 0.0;
        return 60.0 * rate / (best + offset);
    }

    //==============================================================================
    struct Key
    {
        int note = -1;          // 0 = C ... 11 = B; -1: not found
        bool minor = false;
        float confidence = 0.0f;   // how well the best key fits (1 = perfectly)

        bool found() const      { return note >= 0; }

        juce::String toString() const
        {
            static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            return found() ? juce::String (names[note]) + (minor ? " min" : " maj") : juce::String();
        }
    };

    /** A sound's key: how strong each of the 12 notes is in it (55 Hz .. 2 kHz), compared
        with what a major and a minor key of each note usually hold (Krumhansl's profiles).
        Only the first 20 s are looked at; not found for silence, noise or drums. */
    inline Key estimateKey (const float* mono, int numSamples, double sampleRate)
    {
        constexpr int order = 12, size = 1 << order;

        if (mono == nullptr || sampleRate <= 0.0)
            return {};

        // Around 11 kHz is plenty for notes up to 2 kHz, and a 4096 window then tells
        // neighbouring low notes apart.
        const int step = juce::jmax (1, (int) (sampleRate / 11025.0 + 0.5));
        const double rate = sampleRate / step;
        const int length = juce::jmin (numSamples / step, (int) (rate * 20.0));

        if (length < size)
            return {};

        std::vector<float> slow ((size_t) length);

        for (int i = 0; i < length; ++i)
        {
            float sum = 0.0f;

            for (int j = 0; j < step; ++j)
                sum += mono[i * step + j];

            slow[(size_t) i] = sum / (float) step;
        }

        juce::dsp::FFT fft (order);
        std::vector<float> frame ((size_t) size * 2);
        double chroma[12] = {};
        int noteOfBin[size / 2];

        for (int b = 0; b < size / 2; ++b)
        {
            const double hz = b * rate / size;
            noteOfBin[b] = hz < 55.0 || hz > 2000.0 ? -1
                                                    : ((juce::roundToInt (69.0 + 12.0 * std::log2 (hz / 440.0)) % 12) + 12) % 12;
        }

        for (int start = 0; start + size <= length; start += size / 2)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);

            for (int i = 0; i < size; ++i)
                frame[(size_t) i] = slow[(size_t) (start + i)]
                                      * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size));

            fft.performFrequencyOnlyForwardTransform (frame.data());

            for (int b = 1; b < size / 2; ++b)
                if (noteOfBin[b] >= 0)
                    chroma[noteOfBin[b]] += (double) frame[(size_t) b] * frame[(size_t) b];
        }

        double total = 0.0;

        for (auto& c : chroma)
        {
            total += c;
            c = std::sqrt (c);
        }

        if (total < 1.0e-6)
            return {};

        static constexpr double majorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static constexpr double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

        auto fit = [&chroma] (int key, const double* profile)
        {
            double meanA = 0.0, meanB = 0.0;

            for (int i = 0; i < 12; ++i)
            {
                meanA += chroma[(i + key) % 12] / 12.0;
                meanB += profile[i] / 12.0;
            }

            double both = 0.0, a2 = 0.0, b2 = 0.0;

            for (int i = 0; i < 12; ++i)
            {
                const double a = chroma[(i + key) % 12] - meanA, b = profile[i] - meanB;
                both += a * b;
                a2 += a * a;
                b2 += b * b;
            }

            return a2 > 0.0 && b2 > 0.0 ? both / std::sqrt (a2 * b2) : 0.0;
        };

        Key best;
        double bestFit = -2.0;

        for (int key = 0; key < 12; ++key)
            for (int m = 0; m < 2; ++m)
                if (const double f = fit (key, m == 1 ? minorProfile : majorProfile); f > bestFit)
                {
                    bestFit = f;
                    best.note = key;
                    best.minor = m == 1;
                }

        best.confidence = (float) bestFit;

        // (A sound with no clear notes - drums, noise - fits every key about as badly.)
        if (bestFit < 0.6)
            best.note = -1;

        return best;
    }
}

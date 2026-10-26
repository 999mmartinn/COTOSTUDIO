#pragma once

#include <JuceHeader.h>
#include <complex>

/**
    The maths of COTOSTUDIO's 7-band parametric EQ, kept apart from the engine so the
    self-test can measure it and the editor can draw the exact curve that you hear.

    Each band is one filter type, built from 1-4 second-order sections (biquads,
    "Audio EQ Cookbook" formulas by R. Bristow-Johnson). The slope ("order") is 12, 24,
    36 or 48 dB per octave for the cuts; for shelves, band pass and notch more sections
    make the shape steeper / narrower. Our own design, written from scratch.
*/
namespace peq
{
    enum Type { off = 0, highPass, lowShelf, peak, notch, bandPass, highShelf, lowPass, numTypes };
    constexpr int numBands = 7;
    constexpr int maxSections = 4;

    inline const char* typeName (int t)
    {
        switch (t)
        {
            case highPass:  return "Low cut (high pass)";
            case lowShelf:  return "Low shelf";
            case peak:      return "Peaking";
            case notch:     return "Notch (band cut)";
            case bandPass:  return "Band pass";
            case highShelf: return "High shelf";
            case lowPass:   return "High cut (low pass)";
            default:        return "Off";
        }
    }

    inline bool hasGain (int t)        { return t == lowShelf || t == peak || t == highShelf; }
    inline int dbPerOctave (int order) { return 12 * (juce::jlimit (0, 3, order) + 1); }

    /** One band's settings. */
    struct Band
    {
        int type = off;
        int order = 0;            // 0..3 = 12, 24, 36, 48 dB/octave
        double freq = 1000.0;     // Hz
        double gain = 0.0;        // dB (shelves and peaking only)
        double q = 0.707;

        bool operator== (const Band& o) const
        {
            return type == o.type && order == o.order && freq == o.freq && gain == o.gain && q == o.q;
        }

        /** True if it changes the sound at all (otherwise it is skipped: no CPU). */
        bool isActive() const    { return type != off && (! hasGain (type) || std::abs (gain) > 0.001); }
    };

    struct Coeffs
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;   // normalised (a0 = 1)

        std::complex<double> response (double w) const
        {
            const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
            return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
        }
    };

    /** The sections making up a band. */
    struct Design
    {
        std::array<Coeffs, maxSections> sections;
        int numSections = 0;
    };

    inline Design design (const Band& b, double sampleRate)
    {
        Design d;

        if (! b.isActive())
            return d;

        const double nyquistSafe = sampleRate * 0.49;
        const double f = juce::jlimit (5.0, nyquistSafe, b.freq);
        const double q = juce::jlimit (0.025, 40.0, b.q);
        const int n = hasGain (b.type) && b.type == peak ? 1 : juce::jlimit (0, 3, b.order) + 1;
        const double w0 = juce::MathConstants<double>::twoPi * f / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0);

        auto add = [&d] (double b0, double b1, double b2, double a0, double a1, double a2)
        {
            auto& c = d.sections[(size_t) d.numSections++];
            c = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        };

        for (int k = 0; k < n; ++k)
        {
            double sectionQ = q;

            if (b.type == highPass || b.type == lowPass)
            {
                // Butterworth pole pairs; the band's Q sets the resonance of the sharpest one.
                const double theta = juce::MathConstants<double>::pi * (2.0 * (k + 1) - 1.0) / (4.0 * n);
                sectionQ = 1.0 / (2.0 * std::cos (theta));

                if (k == n - 1)
                    sectionQ *= q / 0.70710678;
            }

            const double alpha = sw / (2.0 * sectionQ);

            switch (b.type)
            {
                case highPass:
                    add ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
                    break;

                case lowPass:
                    add ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
                    break;

                case bandPass:   // 0 dB at the centre
                    add (alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
                    break;

                case notch:
                    add (1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);
                    break;

                case peak:
                {
                    const double A = std::pow (10.0, b.gain / 40.0);
                    add (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
                    break;
                }

                case lowShelf:
                case highShelf:
                {
                    const double A = std::pow (10.0, b.gain / n / 40.0);   // the gain is shared by the sections
                    const double s = 2.0 * std::sqrt (A) * alpha;

                    if (b.type == lowShelf)
                        add (A * ((A + 1) - (A - 1) * cw + s), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - s),
                             (A + 1) + (A - 1) * cw + s, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - s);
                    else
                        add (A * ((A + 1) + (A - 1) * cw + s), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - s),
                             (A + 1) - (A - 1) * cw + s, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - s);
                    break;
                }

                default:
                    break;
            }
        }

        return d;
    }

    /** The whole EQ's gain at a frequency, in dB (for the editor's curve and the self-test). */
    inline double responseDb (const std::array<Band, numBands>& bands, double mainDb, double freq, double sampleRate)
    {
        const double w = juce::MathConstants<double>::twoPi * freq / sampleRate;
        double mag = 1.0;

        for (auto& b : bands)
        {
            auto d = design (b, sampleRate);

            for (int i = 0; i < d.numSections; ++i)
                mag *= std::abs (d.sections[(size_t) i].response (w));
        }

        return 20.0 * std::log10 (juce::jmax (1.0e-12, mag)) + mainDb;
    }

    inline double bandResponseDb (const Band& b, double freq, double sampleRate)
    {
        const double w = juce::MathConstants<double>::twoPi * freq / sampleRate;
        auto d = design (b, sampleRate);
        double mag = 1.0;

        for (int i = 0; i < d.numSections; ++i)
            mag *= std::abs (d.sections[(size_t) i].response (w));

        return 20.0 * std::log10 (juce::jmax (1.0e-12, mag));
    }

    //==============================================================================
    /**
        Runs the 7 bands on audio (up to 2 channels). Settings may change at any time:
        they glide to the new values over a few milliseconds (no clicks while you drag),
        and the filters are recalculated every 32 samples while gliding.
    */
    class Processor
    {
    public:
        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate > 0 ? newSampleRate : 44100.0;
            // ~20 ms glide, applied once per 32-sample chunk.
            glide = 1.0 - std::exp (-(double) chunk / (0.02 * sampleRate));
            first = true;
            reset();
        }

        void reset()
        {
            for (auto& b : state)
                for (auto& s : b)
                    s = {};

            current = target;
            mainGain = mainTarget;
        }

        /** New settings (what the knobs say now). */
        void setTarget (const std::array<Band, numBands>& bands, double mainDb)
        {
            target = bands;
            mainTarget = juce::Decibels::decibelsToGain (mainDb);

            if (first)
            {
                current = target;
                mainGain = mainTarget;
                redesignAll();
                first = false;
            }
        }

        /** Hear only one band's range (-1 = normal). Only on this PC, while Shift is held. */
        void setSoloBand (int band)        { soloBand = band; }

        void process (float* const* channels, int numChannels, int numSamples)
        {
            numChannels = juce::jmin (2, numChannels);

            for (int pos = 0; pos < numSamples; pos += chunk)
            {
                const int n = juce::jmin (chunk, numSamples - pos);
                stepTowardsTarget();

                const double g0 = mainGain;
                mainGain += (mainTarget - mainGain) * glide;
                const double gStep = (mainGain - g0) / n;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    float* x = channels[ch] + pos;

                    if (soloBand >= 0)
                    {
                        runSections (soloDesign, soloState[(size_t) ch], x, n);
                        continue;
                    }

                    for (int b = 0; b < numBands; ++b)
                        if (designs[(size_t) b].numSections > 0)
                            runSections (designs[(size_t) b], state[(size_t) b * 2 + (size_t) ch], x, n);

                    double g = g0;

                    for (int i = 0; i < n; ++i)
                    {
                        g += gStep;
                        x[i] = (float) (x[i] * g);
                    }
                }
            }
        }

        double getSampleRate() const     { return sampleRate; }

    private:
        struct SectionState { double z1 = 0, z2 = 0; };
        using BandState = std::array<SectionState, maxSections>;

        static constexpr int chunk = 32;
        double sampleRate = 44100.0, glide = 0.1;
        std::array<Band, numBands> target {}, current {};
        std::array<Design, numBands> designs {};
        std::array<BandState, numBands * 2> state {};
        double mainGain = 1.0, mainTarget = 1.0;
        bool first = true;

        std::atomic<int> soloBand { -1 };
        int designedSolo = -2;
        Design soloDesign;
        std::array<BandState, 2> soloState {};

        static void runSections (const Design& d, BandState& st, float* x, int n)
        {
            for (int s = 0; s < d.numSections; ++s)
            {
                const auto& c = d.sections[(size_t) s];
                auto& z = st[(size_t) s];
                double z1 = z.z1, z2 = z.z2;

                for (int i = 0; i < n; ++i)
                {
                    const double in = x[i];
                    const double out = c.b0 * in + z1;           // transposed direct form II
                    z1 = c.b1 * in - c.a1 * out + z2;
                    z2 = c.b2 * in - c.a2 * out;
                    x[i] = (float) out;
                }

                // Flush denormals so silence never costs CPU.
                z.z1 = std::abs (z1) < 1.0e-20 ? 0.0 : z1;
                z.z2 = std::abs (z2) < 1.0e-20 ? 0.0 : z2;
            }
        }

        void redesignAll()
        {
            for (int b = 0; b < numBands; ++b)
                designs[(size_t) b] = design (current[(size_t) b], sampleRate);
        }

        void stepTowardsTarget()
        {
            for (int b = 0; b < numBands; ++b)
            {
                auto& c = current[(size_t) b];
                const auto& t = target[(size_t) b];

                if (c == t)
                    continue;

                const bool shapeChanged = c.type != t.type || c.order != t.order;

                if (shapeChanged)
                {
                    // A new filter shape starts from silence-free state: clear its memory.
                    c = t;

                    for (auto* st : { &state[(size_t) b * 2], &state[(size_t) b * 2 + 1] })
                        for (auto& s : *st)
                            s = {};
                }
                else
                {
                    // Glide in musical units: frequency and Q in octaves, gain in dB.
                    auto near = [] (double a, double b2, double eps) { return std::abs (a - b2) < eps; };
                    c.freq = std::exp (std::log (c.freq) + (std::log (t.freq) - std::log (c.freq)) * glide);
                    c.q    = std::exp (std::log (c.q)    + (std::log (t.q)    - std::log (c.q))    * glide);
                    c.gain = c.gain + (t.gain - c.gain) * glide;

                    if (near (std::log (c.freq), std::log (t.freq), 1.0e-4)) c.freq = t.freq;
                    if (near (std::log (c.q), std::log (t.q), 1.0e-4))       c.q = t.q;
                    if (near (c.gain, t.gain, 1.0e-3))                         c.gain = t.gain;
                }

                designs[(size_t) b] = design (c, sampleRate);
            }

            const int solo = soloBand.load();

            if (solo != designedSolo || (solo >= 0 && ! (soloSource == current[(size_t) solo])))
            {
                designedSolo = solo;

                if (solo >= 0)
                {
                    // Hear the band's range: a narrow band pass at its frequency and width
                    // (for cuts: the part they remove, around the cut-off).
                    soloSource = current[(size_t) solo];
                    Band b;
                    b.type = bandPass;
                    b.order = 1;
                    b.freq = soloSource.freq;
                    b.q = hasGain (soloSource.type) || soloSource.type == notch || soloSource.type == bandPass
                            ? juce::jmax (0.5, soloSource.q) : 1.0;
                    soloDesign = design (b, sampleRate);
                }
            }
        }

        Band soloSource;
    };
}

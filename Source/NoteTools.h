#pragma once

#include "SongModel.h"

/**
    The piano roll's "Tools" menu (2.3): things done to the selected notes in one go, like
    FL's piano roll tools. Every change is a normal song edit (shared, undoable).

      - Chords:     stamps a chord on top of each note (the note is the root).
      - Arpeggio:   notes that start together (a chord) become a run of single notes,
                    one per grid step, for as long as the chord lasted.
      - Chop:       each note is cut into pieces one grid step long (hi-hat rolls).
      - Strum:      the notes of a chord start one after the other, like a guitar.
      - Humanize:   small random changes to when notes start and how hard they hit.
      - Flip, reverse, half / double length, and a velocity ramp (a roll that grows).
*/
namespace notetools
{
    namespace ids = song::ids;

    constexpr double shortest = song::stepBeats / 4.0;   // (song::addNote's minimum)

    struct Chord
    {
        const char* name;
        int count;
        int steps[4];   // semitones above the note
    };

    inline const Chord chords[] =
    {
        { NEEDS_TRANS ("Major"),                 2, { 4, 7 } },
        { NEEDS_TRANS ("Minor"),                 2, { 3, 7 } },
        { NEEDS_TRANS ("Power (5th + octave)"),  2, { 7, 12 } },
        { NEEDS_TRANS ("Octave"),                1, { 12 } },
        { NEEDS_TRANS ("Sus2"),                  2, { 2, 7 } },
        { NEEDS_TRANS ("Sus4"),                  2, { 5, 7 } },
        { NEEDS_TRANS ("Diminished"),            2, { 3, 6 } },
        { NEEDS_TRANS ("Augmented"),             2, { 4, 8 } },
        { NEEDS_TRANS ("Major 7th"),             3, { 4, 7, 11 } },
        { NEEDS_TRANS ("Minor 7th"),             3, { 3, 7, 10 } },
        { NEEDS_TRANS ("Dominant 7th"),          3, { 4, 7, 10 } },
        { NEEDS_TRANS ("Major 9th"),             4, { 4, 7, 11, 14 } },
        { NEEDS_TRANS ("Minor 9th"),             4, { 3, 7, 10, 14 } },
    };

    constexpr int numChords = (int) (sizeof (chords) / sizeof (chords[0]));

    inline double startOf (const juce::ValueTree& n)     { return (double) n[ids::b]; }
    inline double lengthOf (const juce::ValueTree& n)    { return (double) n[ids::l]; }
    inline int pitchOf (const juce::ValueTree& n)        { return (int) n[ids::p]; }
    inline int velocityOf (const juce::ValueTree& n)     { return (int) n.getProperty (ids::v, 100); }

    /** The notes in groups that start together (chords), earliest first; each group low to high. */
    inline std::vector<std::vector<juce::ValueTree>> chordGroups (const juce::Array<juce::ValueTree>& notes)
    {
        std::vector<juce::ValueTree> sorted;

        for (auto& n : notes)
            sorted.push_back (n);

        std::stable_sort (sorted.begin(), sorted.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
        {
            return startOf (a) < startOf (b);
        });

        std::vector<std::vector<juce::ValueTree>> groups;

        for (auto& n : sorted)
        {
            if (groups.empty() || startOf (n) - startOf (groups.back().front()) > 1.0e-6)
                groups.push_back ({});

            groups.back().push_back (n);
        }

        for (auto& g : groups)
            std::stable_sort (g.begin(), g.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
            {
                return pitchOf (a) < pitchOf (b);
            });

        return groups;
    }

    /** Adds the chord's other notes above each note. Returns the notes it added. */
    inline juce::Array<juce::ValueTree> addChord (te::Edit& edit, juce::ValueTree pattern, const juce::String& channelID,
                                                  const juce::Array<juce::ValueTree>& notes, int chordIndex,
                                                  int lowestPitch, int highestPitch)
    {
        juce::Array<juce::ValueTree> added;
        const auto& chord = chords[juce::jlimit (0, numChords - 1, chordIndex)];

        // (What is there now: a chord note that already exists isn't added twice.)
        const auto existing = song::getNotes (pattern, channelID);

        auto alreadyThere = [&] (int pitch, double beat)
        {
            for (auto& n : existing)
                if (pitchOf (n) == pitch && std::abs (startOf (n) - beat) < 1.0e-6)
                    return true;

            for (auto& n : added)
                if (pitchOf (n) == pitch && std::abs (startOf (n) - beat) < 1.0e-6)
                    return true;

            return false;
        };

        for (auto& n : notes)
            for (int i = 0; i < chord.count; ++i)
            {
                const int pitch = pitchOf (n) + chord.steps[i];

                if (pitch < lowestPitch || pitch > highestPitch || alreadyThere (pitch, startOf (n)))
                    continue;

                auto made = song::addNote (edit, pattern, channelID, pitch, startOf (n), lengthOf (n), velocityOf (n));
                song::copyNoteProperties (edit, n, made);
                added.add (made);
            }

        return added;
    }

    enum class Arp { up, down, upDown };

    /** Chords (2 or more notes starting together) become runs of single notes, one per
        step. Returns the notes that are there afterwards (the new ones, and the single
        notes left as they were). */
    inline juce::Array<juce::ValueTree> arpeggiate (te::Edit& edit, juce::ValueTree pattern, const juce::String& channelID,
                                                    const juce::Array<juce::ValueTree>& notes, double step, Arp mode)
    {
        juce::Array<juce::ValueTree> result;
        step = juce::jmax (shortest, step);

        for (auto& group : chordGroups (notes))
        {
            if (group.size() < 2)
            {
                for (auto& n : group)
                    result.add (n);

                continue;
            }

            std::vector<juce::ValueTree> order (group);   // low to high

            if (mode == Arp::down)
                std::reverse (order.begin(), order.end());
            else if (mode == Arp::upDown)
                for (int i = (int) group.size() - 2; i >= 1; --i)
                    order.push_back (group[(size_t) i]);

            const double start = startOf (group.front());
            double end = start;

            for (auto& n : group)
                end = juce::jmax (end, startOf (n) + lengthOf (n));

            for (int i = 0; i < 4096; ++i)
            {
                const double t = start + i * step;

                if (t >= end - 1.0e-6)
                    break;

                const auto& from = order[(size_t) i % order.size()];
                auto made = song::addNote (edit, pattern, channelID, pitchOf (from), t, juce::jmin (step, end - t), velocityOf (from));
                song::copyNoteProperties (edit, from, made);
                result.add (made);
            }

            for (auto& n : group)
                song::removeNote (edit, n);
        }

        return result;
    }

    /** Each note longer than a step is cut into pieces a step long. Returns the notes
        that are there afterwards. */
    inline juce::Array<juce::ValueTree> chop (te::Edit& edit, juce::ValueTree pattern, const juce::String& channelID,
                                              const juce::Array<juce::ValueTree>& notes, double step)
    {
        juce::Array<juce::ValueTree> result;
        step = juce::jmax (shortest, step);

        for (auto& n : notes)
        {
            const double start = startOf (n), end = start + lengthOf (n);

            if (end - start <= step + 1.0e-6)
            {
                result.add (n);
                continue;
            }

            for (int i = 0; i < 4096; ++i)
            {
                const double t = start + i * step;

                if (t >= end - 1.0e-6)
                    break;

                auto made = song::addNote (edit, pattern, channelID, pitchOf (n), t, juce::jmin (step, end - t), velocityOf (n));
                song::copyNoteProperties (edit, n, made);
                result.add (made);
            }

            song::removeNote (edit, n);
        }

        return result;
    }

    /** The notes of each chord start one after the other (`amount` beats apart), from the
        lowest up, or from the highest down; they still end where they ended. */
    inline void strum (te::Edit& edit, const juce::Array<juce::ValueTree>& notes, double amount, bool downwards, double patternLength)
    {
        auto* um = &edit.getUndoManager();

        for (auto group : chordGroups (notes))
        {
            if (downwards)
                std::reverse (group.begin(), group.end());

            for (size_t i = 1; i < group.size(); ++i)
            {
                auto n = group[i];
                const double b = startOf (n), l = lengthOf (n);
                const double shift = juce::jmin (amount * (double) i, juce::jmax (0.0, l - shortest),
                                                 juce::jmax (0.0, patternLength - shortest - b));

                if (shift > 1.0e-9)
                {
                    n.setProperty (ids::b, b + shift, um);
                    n.setProperty (ids::l, l - shift, um);
                }
            }
        }
    }

    /** Small random changes: starts by up to +-`timing` beats, velocities by up to +-`velocity`. */
    inline void humanize (te::Edit& edit, const juce::Array<juce::ValueTree>& notes, juce::Random& random,
                          double timing, int velocity, double patternLength)
    {
        auto* um = &edit.getUndoManager();

        for (auto n : notes)
        {
            if (timing > 0.0)
            {
                const double b = startOf (n);
                const double moved = juce::jlimit (0.0, juce::jmax (0.0, patternLength - shortest),
                                                   b + (random.nextDouble() * 2.0 - 1.0) * timing);

                if (std::abs (moved - b) > 1.0e-9)
                    n.setProperty (ids::b, moved, um);
            }

            if (velocity > 0)
            {
                const int v = velocityOf (n);
                const int changed = juce::jlimit (1, 127, v + random.nextInt (juce::Range<int> (-velocity, velocity + 1)));

                if (changed != v)
                    n.setProperty (ids::v, changed, um);
            }
        }
    }

    /** Upside down: the highest note becomes the lowest and so on. */
    inline void flip (te::Edit& edit, const juce::Array<juce::ValueTree>& notes)
    {
        if (notes.isEmpty())
            return;

        int low = 127, high = 0;

        for (auto& n : notes)
        {
            low = juce::jmin (low, pitchOf (n));
            high = juce::jmax (high, pitchOf (n));
        }

        for (auto n : notes)
            if (const int p = low + high - pitchOf (n); p != pitchOf (n))
                n.setProperty (ids::p, p, &edit.getUndoManager());
    }

    /** Back to front: the last note comes first. */
    inline void reverse (te::Edit& edit, const juce::Array<juce::ValueTree>& notes)
    {
        if (notes.isEmpty())
            return;

        double start = 1.0e9, end = 0.0;

        for (auto& n : notes)
        {
            start = juce::jmin (start, startOf (n));
            end = juce::jmax (end, startOf (n) + lengthOf (n));
        }

        for (auto n : notes)
        {
            const double b = juce::jmax (0.0, start + (end - (startOf (n) + lengthOf (n))));

            if (std::abs (b - startOf (n)) > 1.0e-9)
                n.setProperty (ids::b, b, &edit.getUndoManager());
        }
    }

    /** Every note `factor` times as long (0.5 = half, 2 = double), inside the pattern. */
    inline void scaleLengths (te::Edit& edit, const juce::Array<juce::ValueTree>& notes, double factor, double patternLength)
    {
        for (auto n : notes)
        {
            const double l = lengthOf (n);
            const double wanted = juce::jlimit (shortest, juce::jmax (shortest, patternLength - startOf (n)), l * factor);

            if (std::abs (wanted - l) > 1.0e-9)
                n.setProperty (ids::l, wanted, &edit.getUndoManager());
        }
    }

    /** Velocities going evenly from `first` (the earliest note) to `last` (the latest). */
    inline void velocityRamp (te::Edit& edit, const juce::Array<juce::ValueTree>& notes, int first, int last)
    {
        if (notes.isEmpty())
            return;

        double start = 1.0e9, end = 0.0;

        for (auto& n : notes)
        {
            start = juce::jmin (start, startOf (n));
            end = juce::jmax (end, startOf (n));
        }

        for (auto n : notes)
        {
            const double along = end > start ? (startOf (n) - start) / (end - start) : 1.0;
            const int v = juce::jlimit (1, 127, juce::roundToInt (first + (last - first) * along));

            if (v != velocityOf (n))
                n.setProperty (ids::v, v, &edit.getUndoManager());
        }
    }
}

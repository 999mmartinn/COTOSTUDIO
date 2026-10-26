#pragma once

#include <JuceHeader.h>

/**
    Musical scales for the piano roll's "Scale" helper: with a key and a scale chosen
    (and snapping on), new and moved notes land only on notes of that scale.
    Pitches are MIDI note numbers; C5 = 60 (FL naming).
*/
namespace scales
{
    struct Type
    {
        const char* name;
        int count;
        int steps[8];   // semitones above the key note
    };

    inline const Type types[] =
    {
        { "Major",             7, { 0, 2, 4, 5, 7, 9, 11 } },
        { "Minor",             7, { 0, 2, 3, 5, 7, 8, 10 } },
        { "Harmonic minor",    7, { 0, 2, 3, 5, 7, 8, 11 } },
        { "Melodic minor",     7, { 0, 2, 3, 5, 7, 9, 11 } },
        { "Dorian",            7, { 0, 2, 3, 5, 7, 9, 10 } },
        { "Phrygian",          7, { 0, 1, 3, 5, 7, 8, 10 } },
        { "Lydian",            7, { 0, 2, 4, 6, 7, 9, 11 } },
        { "Mixolydian",        7, { 0, 2, 4, 5, 7, 9, 10 } },
        { "Locrian",           7, { 0, 1, 3, 5, 6, 8, 10 } },
        { "Major pentatonic",  5, { 0, 2, 4, 7, 9 } },
        { "Minor pentatonic",  5, { 0, 3, 5, 7, 10 } },
        { "Blues",             6, { 0, 3, 5, 6, 7, 10 } },
    };

    constexpr int numTypes = (int) (sizeof (types) / sizeof (types[0]));

    inline const char* keyName (int key)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names[((key % 12) + 12) % 12];
    }

    inline const Type& type (int index)             { return types[juce::jlimit (0, numTypes - 1, index)]; }

    /** True if a pitch belongs to the scale of this key. */
    inline bool contains (int key, int typeIndex, int pitch)
    {
        const auto& t = type (typeIndex);
        const int degree = (((pitch - key) % 12) + 12) % 12;

        for (int i = 0; i < t.count; ++i)
            if (t.steps[i] == degree)
                return true;

        return false;
    }

    /** True if a pitch is the key note itself (any octave). */
    inline bool isKeyNote (int key, int pitch)       { return (((pitch - key) % 12) + 12) % 12 == 0; }

    /** The scale note nearest to a pitch. When two are equally near, `preferUp`
        decides (dragging a note up should reach the next note up). */
    inline int nearest (int key, int typeIndex, int pitch, bool preferUp = false, int lowest = 0, int highest = 127)
    {
        for (int distance = 0; distance < 12; ++distance)
        {
            const int first = preferUp ? pitch + distance : pitch - distance;
            const int second = preferUp ? pitch - distance : pitch + distance;

            if (first >= lowest && first <= highest && contains (key, typeIndex, first))   return first;
            if (second >= lowest && second <= highest && contains (key, typeIndex, second)) return second;
        }

        return pitch;
    }

    /** "C minor", "F# major pentatonic"... */
    inline juce::String describe (int key, int typeIndex)
    {
        return juce::String (keyName (key)) + " " + TRANS (type (typeIndex).name).toLowerCase();
    }
}

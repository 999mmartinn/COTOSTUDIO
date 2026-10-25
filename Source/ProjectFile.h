#pragma once

#include <JuceHeader.h>
#include "AssetStore.h"

/**
    A saved song: one ".syncsong" file (a zip) that holds
      - song.xml: the song (everything everyone shares: channels, patterns, notes,
        playlist, mixer, plugin settings, tempo...);
      - info.xml: which version saved it, when and who;
      - audio/<fingerprint>.flac: every audio file the song uses. Autosave backups
        leave these out (they are in this PC's audio folder anyway).

    Audio files are checked against their fingerprint when the song is opened, so a
    damaged file never gets into the song. Both functions only touch the disk and
    the given data, so they can run on a background thread.
*/
namespace project
{
    constexpr int formatVersion = 1;
    inline const char* fileExtension = ".syncsong";

    struct SaveResult
    {
        juce::String error;
        int audioIncluded = 0, audioMissing = 0;   // missing: still arriving on this PC
        juce::int64 bytes = 0;
        juce::int64 songHash = 0;                  // fingerprint of the song exactly as saved (set by the Session)
        bool ok() const noexcept                   { return error.isEmpty(); }
    };

    /** Writes the song (and its audio files, if asked) to a file. The old file is only
        replaced once the new one is complete. */
    SaveResult save (const juce::File& target, const juce::ValueTree& song, const AssetStore&,
                     const juce::StringArray& audioHashes, bool includeAudio, const juce::String& savedBy);

    struct LoadResult
    {
        juce::String error;
        juce::ValueTree song;
        int audioRestored = 0, audioAlreadyHere = 0, audioDamaged = 0;
        bool ok() const noexcept                   { return error.isEmpty() && song.isValid(); }
    };

    /** Reads a song file and puts its audio files into the asset store (verified). */
    LoadResult load (const juce::File& source, AssetStore&);
}

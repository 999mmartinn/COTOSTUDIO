#pragma once

#include <JuceHeader.h>
#include "AssetStore.h"

/**
    A saved song is a project folder:

      <song>\<song>.cotosong   the song (a small zip):
                                 - song.xml: everything everyone shares (channels, patterns,
                                   notes, playlist, mixer, plugin settings, tempo...)
                                 - info.xml: which version saved it, when and who
                                 - audio.xml: which audio file is which (by fingerprint)
      <song>\Audio\            the sounds it uses, with readable names ("Kick Hard.flac")
      <song>\Takes\            its recordings ("Vocals take 1.flac")

    Saving again reuses the files that are already there (nothing is copied twice);
    on the same drive the files share their bytes with the app's working copy, so
    they take no extra space. Files the song no longer uses go to the recycle bin.

    Opening checks every audio file against its fingerprint, so a damaged or edited
    file never gets into the song. Older ".syncsong" files (one zip with the audio
    inside) still open. Both functions only touch the disk and the given data, so
    they can run on a background thread.
*/
namespace project
{
    constexpr int formatVersion = 2;

    /** One audio file of the song, as the song calls it. */
    struct AudioFile
    {
        juce::String hash, name;   // name: e.g. "Kick Hard.wav", "Vocals take 1.flac"
        bool isTake = false;
    };

    struct SaveResult
    {
        juce::String error;
        int audioIncluded = 0, audioMissing = 0;   // missing: still arriving on this PC
        int audioWritten = 0;                      // new files written this time (the rest were there already)
        juce::int64 bytes = 0;
        juce::int64 songHash = 0;                  // fingerprint of the song exactly as saved (set by the Session)
        bool ok() const noexcept                   { return error.isEmpty(); }
    };

    /** Writes the song file (replacing the old one only once the new one is complete) and,
        if asked, its audio into the Audio / Takes folders next to it.
        alreadyThere: files in that folder known to hold a sound (path -> fingerprint), so they
        are reused without being read again (backups: from the folder's other backups).
        tidy: files the last save of this same song file put there and that it no longer
        uses go to the recycle bin (off for backups: the other backups may use them). */
    SaveResult save (const juce::File& target, const juce::ValueTree& song, const AssetStore&,
                     const std::vector<AudioFile>& audio, bool includeAudio, const juce::String& savedBy,
                     const std::map<juce::String, juce::String>& alreadyThere = {}, bool tidy = true);

    struct LoadResult
    {
        juce::String error;
        juce::ValueTree song;
        int audioRestored = 0, audioAlreadyHere = 0, audioDamaged = 0, audioMissing = 0;
        bool ok() const noexcept                   { return error.isEmpty() && song.isValid(); }
    };

    /** Reads a song file and puts its audio files into the asset store (verified). */
    LoadResult load (const juce::File& source, AssetStore&);

    /** Which audio file is which in a saved song: path (relative to the song file) -> fingerprint. */
    std::map<juce::String, juce::String> readAudioList (const juce::File& songFile);
}

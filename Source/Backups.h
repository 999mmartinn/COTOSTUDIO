#pragma once

#include <JuceHeader.h>
#include <map>
#include <set>

/**
    Automatic backups (the main window makes one every few minutes while the song changes).

    Each backup is a complete song file with its audio, so it opens even after the app's
    working copies of the audio are gone (a never-saved song's are deleted when you close
    it without saving). The audio of all the backups of one song shares one Audio\ and
    Takes\ folder next to them, and on the same drive those are second names for the
    working copies (hard links), so they take no extra space.

      saved song:        <project>\Backups\<song> (backup <date time>).cotosong
      never-saved song:  <data>\Backups\<song> (started <date time>)\<song> (backup <date time>).cotosong

    The newest `keepPerSong` backups of each song are kept (and the audio only they used is
    deleted with the older ones); a never-saved song's backups go after `unsavedDays` days.
*/
namespace backups
{
    constexpr int keepPerSong = 20, unsavedDays = 14;

    /** The folder for a never-saved song's backups (one per song, by its name and when it started). */
    juce::File folderForUnsaved (const juce::String& songTitle, juce::Time started);

    /** A new backup's file name in a folder. */
    juce::File newBackupFile (const juce::File& folder, const juce::String& songTitle);

    /** The audio the folder's backups already hold (path -> fingerprint, from their lists). */
    std::map<juce::String, juce::String> knownAudio (const juce::File& folder);

    /** Keeps the newest backups in a folder and deletes the audio none of the rest uses.
        (Disk only: fine on a background thread.) */
    void prune (const juce::File& folder, int keep = keepPerSong);

    /** Deletes never-saved songs' backup folders whose newest backup is older than this. */
    void pruneOldUnsaved (int days = unsavedDays);

    struct Entry
    {
        juce::File file;
        juce::String song;
        juce::Time when;
        bool thisSong = false;
    };

    /** Every backup: this song's (in currentFolder) first, then the never-saved songs', newest first. */
    juce::Array<Entry> list (const juce::File& currentFolder);

    /** "5 min ago", "yesterday 21:43"... */
    juce::String describeWhen (juce::Time);
}

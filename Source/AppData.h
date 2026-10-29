#pragma once

#include <JuceHeader.h>
#include <map>
#include <set>

/**
    Where COTOSTUDIO keeps everything, in one folder (the "data folder"):

      <data folder>                 D:\COTOSTUDIO if that folder exists, otherwise
                                    Documents\COTOSTUDIO (can be changed in Settings)
        Projects\<song>\            one folder per song:
            <song>.cotosong           the song itself
            Audio\                    the sounds it uses (imported files, samples)
            Takes\                    its recordings
            Exports\                  what you exported from it
            Backups\                  its automatic backups
        Templates\<name>\           songs to start new ones from (File > Templates)
        Backups\                    backups of songs that were never saved
        Exports\                    exports of songs that were never saved
        Settings\                   your settings, plugin list and audio device
        Cache\                      working files: the shared audio of sessions
                                    (by fingerprint), takes being recorded, engine temp

    Nothing else is written to C:. Only if you choose another data folder is a
    one-line note of where it is kept in %APPDATA%\COTOSTUDIO.
*/
namespace appdata
{
    inline const char* appName = "COTOSTUDIO";
    inline const char* songExtension = ".cotosong";
    inline const char* oldSongExtension = ".syncsong";
    inline const char* songWildcards = "*.cotosong;*.syncsong";

    //==============================================================================
    /** The data folder (decided once at start-up). */
    juce::File getRoot();
    /** D:\COTOSTUDIO if it exists, else Documents\COTOSTUDIO. */
    juce::File defaultRoot();
    /** Uses another data folder from the next start (remembered in %APPDATA%\COTOSTUDIO). */
    void chooseRoot (const juce::File&);
    /** For tests: use this folder right away. */
    void setRootForThisRun (const juce::File&);

    juce::File projectsFolder();
    juce::File templatesFolder();          // Templates\<name>\<name>.cotosong (+ its Audio)
    /** Templates\<name>\<name>.cotosong */
    juce::File templateFileForName (const juce::String& name);
    /** Every template's song file, by name. */
    juce::Array<juce::File> findTemplates();
    juce::File backupsFolder();            // backups of never-saved songs
    juce::File exportsFolder();            // exports of never-saved songs
    juce::File settingsFolder();
    juce::File cacheFolder();
    /** The shared audio files of copy N of the app on this PC (1 = the normal one). */
    juce::File audioCacheFolder (int copyNumber);

    /** Projects\<name>\<name>.cotosong */
    juce::File songFileForName (const juce::String& name);
    /** Turns any chosen song file into its project form: "<folder>\<name>\<name>.cotosong"
        (unless it already sits in a folder of its own name). */
    juce::File projectSongFile (const juce::File& chosen);
    /** True if a song file is an automatic backup (it sits in a "Backups" folder, or in a
        never-saved song's folder inside the data folder's Backups). */
    bool isBackup (const juce::File& songFile);
    /** The folder of a saved song's project (its Exports, Backups... live here). */
    inline juce::File projectFolderOf (const juce::File& songFile)   { return songFile.getParentDirectory(); }

    /** A second name for the same bytes when both are on one drive (no extra space),
        otherwise a copy. */
    bool linkOrCopy (const juce::File& source, const juce::File& target);

    //==============================================================================
    /** Moving the data of older versions (SyncTest) off C: into the data folder. */
    struct Migration
    {
        struct Move { juce::File from, to; bool isFolder = false; };
        std::vector<Move> moves;
        juce::Array<juce::File> toDeleteAfterwards;   // old folders, once everything is out
        std::map<juce::String, juce::String> renamedSongs;   // old path -> new path (for "recent songs")
        juce::int64 totalBytes = 0;

        bool isNeeded() const              { return ! moves.empty() || ! toDeleteAfterwards.isEmpty(); }
    };

    /** What needs moving: oldAppData = %APPDATA%\SyncTest, oldSongs = Documents\SyncTest Songs. */
    Migration planMigration (const juce::File& oldAppData, const juce::File& oldSongs, const juce::File& newRoot);

    struct MigrationResult
    {
        int filesMoved = 0, failures = 0;
        juce::int64 bytesMoved = 0;
        juce::StringArray problems;
    };

    /** Does it (a background thread is fine). progress gets 0..1. A file is only removed
        from its old place once its copy is complete. */
    MigrationResult runMigration (const Migration&, std::function<void (double)> progress);

    /** The usual old places on this PC. */
    juce::File oldAppDataFolder();
    juce::File oldSongsFolder();
}

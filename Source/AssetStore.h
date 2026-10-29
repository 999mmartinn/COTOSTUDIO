#pragma once

#include <JuceHeader.h>
#include <map>

/**
    This PC's folder of shared audio files ("assets").

    Every file is stored as FLAC (lossless, about half the size of WAV) and
    named by a fingerprint of its content (SHA-256), so:
      - two PCs talking about the same file always mean exactly the same bytes;
      - a received file is checked against its fingerprint before it is used,
        so a broken transfer can never end up in the song.

    Incoming transfers arrive in chunks (see collab::Session); this class
    writes them to "<hash>.part" and turns them into "<hash>.flac" once
    complete and verified.
*/
class AssetStore
{
public:
    explicit AssetStore (juce::File folder);

    void setFolder (juce::File newFolder);
    juce::File getFolder() const                     { return folder; }

    bool has (const juce::String& hash) const;
    juce::File getFile (const juce::String& hash) const;   // may not exist yet

    struct ImportResult
    {
        juce::String hash, error;
        double lengthSeconds = 0.0, sampleRate = 0.0;
        int numChannels = 0;
        juce::int64 bytes = 0;
        bool ok() const noexcept                     { return error.isEmpty() && hash.isNotEmpty(); }
    };

    /** Reads any audio file JUCE understands (WAV, AIFF, FLAC, MP3, OGG) and
        stores it as FLAC. Blocking: call it from a background thread for big files. */
    ImportResult importAudioFile (const juce::File& source);

    static juce::String fingerprint (const juce::File&);

    /** Takes a file that claims to be this asset (e.g. from a saved song) into the store,
        if its content really matches the fingerprint. The file is moved. Only touches
        the disk, so it may be called from a background thread. */
    bool adoptFile (const juce::File& file, const juce::String& hash);

    //==============================================================================
    // Incoming transfers
    enum class ChunkResult { accepted, complete, failed, ignored };

    ChunkResult receiveChunk (const juce::String& hash, juce::int64 offset, juce::int64 total,
                              const juce::MemoryBlock& data, int sourceTag);

    bool isDownloading (const juce::String& hash) const;
    /** 0..1 while downloading, 1 when present, -1 when neither. */
    double getProgress (const juce::String& hash) const;
    /** Abandons transfers that went quiet (sender left) so they can be asked for again. */
    void dropStalledDownloads (juce::uint32 maxSilenceMs);
    void cancelDownloadsFrom (int sourceTag);

    /** Called on the message thread when a new asset becomes available. */
    std::function<void (const juce::String& hash)> onAssetAdded;

    //==============================================================================
    // Clean-up: files that arrived while a song was open (recorded, imported, received,
    // restored) can be deleted when that song is closed without being saved.

    /** Starts a new list of arrivals (a new song is open). */
    void markNewSong();
    /** Files that arrived since the last markNewSong(). Thread-safe. */
    juce::StringArray getAddedSinceMark() const;
    /** Deletes these files from the store. Files that can't be deleted yet (still open,
        e.g. on Windows) are listed in pending-deletes.txt and retried later. */
    void deleteFiles (const juce::StringArray& hashes);
    /** Retries the files listed in a store folder's pending-deletes.txt (e.g. at start-up,
        or after the audio engine has let go of them). */
    static void retryPendingDeletes (const juce::File& storeFolder);

private:
    juce::File folder;
    juce::StringArray addedSinceMark;
    juce::CriticalSection addedLock;
    void noteAdded (const juce::String& hash);

    struct Download
    {
        std::unique_ptr<juce::FileOutputStream> out;
        juce::int64 total = 0, received = 0;
        int source = -1;
        juce::uint32 lastActivity = 0;
    };

    std::map<juce::String, Download> downloads;

    juce::File getPartFile (const juce::String& hash) const;
    void abandon (const juce::String& hash);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AssetStore)
};

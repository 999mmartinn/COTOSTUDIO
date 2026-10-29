#include "AssetStore.h"

AssetStore::AssetStore (juce::File f)
{
    setFolder (f);
}

void AssetStore::setFolder (juce::File newFolder)
{
    for (auto& [hash, d] : downloads)
        d.out.reset();

    downloads.clear();
    folder = newFolder;
    folder.createDirectory();
    retryPendingDeletes (folder);   // left from last time (files that were still open then)
    markNewSong();
}

//==============================================================================
void AssetStore::noteAdded (const juce::String& hash)
{
    const juce::ScopedLock sl (addedLock);
    addedSinceMark.addIfNotAlreadyThere (hash);
}

void AssetStore::markNewSong()
{
    const juce::ScopedLock sl (addedLock);
    addedSinceMark.clear();
}

juce::StringArray AssetStore::getAddedSinceMark() const
{
    const juce::ScopedLock sl (addedLock);
    return addedSinceMark;
}

static juce::File pendingListIn (const juce::File& storeFolder)   { return storeFolder.getChildFile ("pending-deletes.txt"); }

static bool isAssetHash (const juce::String& h)   { return h.length() == 32 && h.containsOnly ("0123456789abcdef"); }

void AssetStore::deleteFiles (const juce::StringArray& hashes)
{
    juce::StringArray stillThere;

    for (auto& h : hashes)
    {
        if (! isAssetHash (h))
            continue;

        auto f = getFile (h);

        if (f.existsAsFile() && ! f.deleteFile())
            stillThere.add (h);   // open somewhere: try again later

        const juce::ScopedLock sl (addedLock);
        addedSinceMark.removeString (h);
    }

    if (! stillThere.isEmpty())
    {
        auto list = pendingListIn (folder);
        juce::StringArray pending;
        pending.addLines (list.loadFileAsString());
        pending.addArray (stillThere);
        pending.removeEmptyStrings();
        pending.removeDuplicates (false);
        list.replaceWithText (pending.joinIntoString ("\n"));
    }
}

void AssetStore::retryPendingDeletes (const juce::File& storeFolder)
{
    auto list = pendingListIn (storeFolder);

    if (! list.existsAsFile())
        return;

    juce::StringArray pending, left;
    pending.addLines (list.loadFileAsString());

    for (auto& h : pending)
    {
        if (! isAssetHash (h.trim()))
            continue;

        auto f = storeFolder.getChildFile (h.trim() + ".flac");

        if (f.existsAsFile() && ! f.deleteFile())
            left.add (h.trim());
    }

    if (left.isEmpty())
        list.deleteFile();
    else
        list.replaceWithText (left.joinIntoString ("\n"));
}

juce::File AssetStore::getFile (const juce::String& hash) const       { return folder.getChildFile (hash + ".flac"); }
juce::File AssetStore::getPartFile (const juce::String& hash) const   { return folder.getChildFile (hash + ".part"); }

bool AssetStore::has (const juce::String& hash) const
{
    return hash.isNotEmpty() && getFile (hash).existsAsFile();
}

juce::String AssetStore::fingerprint (const juce::File& f)
{
    return juce::SHA256 (f).toHexString().substring (0, 32);
}

AssetStore::ImportResult AssetStore::importAudioFile (const juce::File& source)
{
    ImportResult result;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

    if (reader == nullptr)
    {
        result.error = "Can't read " + source.getFileName() + " (unsupported or damaged audio file)";
        return result;
    }

    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        result.error = source.getFileName() + " has no audio";
        return result;
    }

    // Write a FLAC copy into the store under a temporary name...
    auto temp = folder.getNonexistentChildFile ("import", ".tmp", false);
    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);

    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> stream (temp.createOutputStream());

        if (stream == nullptr)
        {
            result.error = "Can't write to " + folder.getFullPathName();
            return result;
        }

        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                        .withSampleRate (reader->sampleRate)
                                                        .withNumChannels (channels)
                                                        .withBitsPerSample (24));

        if (writer == nullptr)
        {
            stream.reset();          // close it first: Windows can't delete an open file
            temp.deleteFile();
            result.error = "Can't convert " + source.getFileName() + " (sample rate " + juce::String (reader->sampleRate) + ")";
            return result;
        }

        if (! writer->writeFromAudioReader (*reader, 0, -1))
        {
            writer.reset();
            temp.deleteFile();
            result.error = "Failed while converting " + source.getFileName();
            return result;
        }
    }

    // ...then name it after its fingerprint.
    result.hash = fingerprint (temp);
    auto finalFile = getFile (result.hash);

    if (finalFile.existsAsFile())
        temp.deleteFile();                      // same content already here
    else if (! temp.moveFileTo (finalFile))
    {
        temp.deleteFile();
        result.error = "Can't store " + source.getFileName();
        return result;
    }
    else
        noteAdded (result.hash);

    result.sampleRate = reader->sampleRate;
    result.numChannels = channels;
    result.lengthSeconds = (double) reader->lengthInSamples / reader->sampleRate;
    result.bytes = finalFile.getSize();
    return result;
}

bool AssetStore::adoptFile (const juce::File& file, const juce::String& hash)
{
    if (hash.length() != 32 || ! hash.containsOnly ("0123456789abcdef") || ! file.existsAsFile())
        return false;

    if (has (hash))
    {
        file.deleteFile();
        return true;   // already here
    }

    if (fingerprint (file) != hash || ! file.moveFileTo (getFile (hash)))
    {
        file.deleteFile();
        return false;
    }

    noteAdded (hash);
    return true;
}

//==============================================================================
AssetStore::ChunkResult AssetStore::receiveChunk (const juce::String& hash, juce::int64 offset, juce::int64 total,
                                                  const juce::MemoryBlock& data, int sourceTag)
{
    if (hash.length() != 32 || ! hash.containsOnly ("0123456789abcdef") || total <= 0)
        return ChunkResult::ignored;

    if (has (hash))
        return ChunkResult::ignored;   // already here (another PC may have sent it too)

    auto it = downloads.find (hash);

    if (it == downloads.end())
    {
        if (offset != 0)
            return ChunkResult::ignored;   // joined in the middle of someone else's transfer

        Download d;
        getPartFile (hash).deleteFile();
        d.out = std::make_unique<juce::FileOutputStream> (getPartFile (hash));

        if (d.out->failedToOpen())
            return ChunkResult::failed;

        d.total = total;
        d.source = sourceTag;
        it = downloads.emplace (hash, std::move (d)).first;
    }

    auto& d = it->second;

    if (d.source != sourceTag || offset != d.received || total != d.total)
        return ChunkResult::ignored;   // a second sender, or a repeat: the first one wins

    if (! d.out->write (data.getData(), data.getSize()))
    {
        abandon (hash);
        return ChunkResult::failed;
    }

    d.received += (juce::int64) data.getSize();
    d.lastActivity = juce::Time::getMillisecondCounter();

    if (d.received < d.total)
        return ChunkResult::accepted;

    // Complete: check the fingerprint before using it.
    d.out->flush();
    d.out.reset();
    downloads.erase (it);

    auto part = getPartFile (hash);

    if (fingerprint (part) != hash)
    {
        part.deleteFile();
        return ChunkResult::failed;
    }

    if (! part.moveFileTo (getFile (hash)))
    {
        part.deleteFile();
        return ChunkResult::failed;
    }

    noteAdded (hash);

    if (onAssetAdded)
        onAssetAdded (hash);

    return ChunkResult::complete;
}

bool AssetStore::isDownloading (const juce::String& hash) const
{
    return downloads.find (hash) != downloads.end();
}

double AssetStore::getProgress (const juce::String& hash) const
{
    if (has (hash))
        return 1.0;

    auto it = downloads.find (hash);

    if (it == downloads.end())
        return -1.0;

    return (double) it->second.received / (double) juce::jmax ((juce::int64) 1, it->second.total);
}

void AssetStore::abandon (const juce::String& hash)
{
    auto it = downloads.find (hash);

    if (it != downloads.end())
    {
        it->second.out.reset();
        downloads.erase (it);
    }

    getPartFile (hash).deleteFile();
}

void AssetStore::dropStalledDownloads (juce::uint32 maxSilenceMs)
{
    const auto now = juce::Time::getMillisecondCounter();
    juce::StringArray stalled;

    for (auto& [hash, d] : downloads)
        if (now - d.lastActivity > maxSilenceMs)
            stalled.add (hash);

    for (auto& h : stalled)
        abandon (h);
}

void AssetStore::cancelDownloadsFrom (int sourceTag)
{
    juce::StringArray fromSource;

    for (auto& [hash, d] : downloads)
        if (d.source == sourceTag)
            fromSource.add (hash);

    for (auto& h : fromSource)
        abandon (h);
}

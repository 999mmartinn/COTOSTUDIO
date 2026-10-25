#include "ProjectFile.h"

namespace project
{

static juce::InputStream* streamOf (const juce::String& text)
{
    juce::MemoryBlock data (text.toRawUTF8(), text.getNumBytesAsUTF8());
    return new juce::MemoryInputStream (std::move (data), true);
}

SaveResult save (const juce::File& target, const juce::ValueTree& song, const AssetStore& assets,
                 const juce::StringArray& audioHashes, bool includeAudio, const juce::String& savedBy)
{
    SaveResult result;

    if (! song.isValid())
    {
        result.error = "There is no song to save";
        return result;
    }

    juce::ZipFile::Builder zip;
    const auto now = juce::Time::getCurrentTime();

    juce::XmlElement info ("SYNCSONG");
    info.setAttribute ("format", formatVersion);
    info.setAttribute ("app", juce::JUCEApplicationBase::getInstance() != nullptr
                                ? juce::JUCEApplicationBase::getInstance()->getApplicationVersion() : juce::String());
    info.setAttribute ("savedBy", savedBy);
    info.setAttribute ("savedAt", now.toISO8601 (true));
    info.setAttribute ("withAudio", includeAudio);

    zip.addEntry (streamOf (info.toString()), 9, "info.xml", now);
    zip.addEntry (streamOf (song.toXmlString()), 9, "song.xml", now);

    if (includeAudio)
    {
        for (auto& hash : audioHashes)
        {
            if (assets.has (hash))
            {
                zip.addFile (assets.getFile (hash), 0, "audio/" + hash + ".flac");   // FLAC is already compressed
                ++result.audioIncluded;
            }
            else
            {
                ++result.audioMissing;
            }
        }
    }

    // Write next to the target, then swap: a failed save never damages the old file.
    target.getParentDirectory().createDirectory();
    auto temp = target.getSiblingFile (target.getFileNameWithoutExtension() + ".saving.tmp");
    temp.deleteFile();

    {
        juce::FileOutputStream out (temp);

        if (out.failedToOpen())
        {
            result.error = "Can't write to " + target.getParentDirectory().getFullPathName();
            return result;
        }

        if (! zip.writeToStream (out, nullptr))
        {
            result.error = "Writing the song file failed (is the disk full?)";
            out.flush();
            temp.deleteFile();
            return result;
        }

        out.flush();

        if (out.getStatus().failed())
        {
            result.error = "Writing the song file failed: " + out.getStatus().getErrorMessage();
            temp.deleteFile();
            return result;
        }
    }

    if (! temp.moveFileTo (target))
    {
        temp.deleteFile();
        result.error = "Can't replace " + target.getFullPathName() + " (is it open somewhere else?)";
        return result;
    }

    result.bytes = target.getSize();
    return result;
}

//==============================================================================
static juce::String readEntry (juce::ZipFile& zip, const juce::String& name)
{
    if (auto* entry = zip.getEntry (name))
        if (std::unique_ptr<juce::InputStream> in { zip.createStreamForEntry (*entry) })
            return in->readEntireStreamAsString();

    return {};
}

LoadResult load (const juce::File& source, AssetStore& assets)
{
    LoadResult result;
    juce::ZipFile zip (source);

    if (! source.existsAsFile() || zip.getNumEntries() == 0)
    {
        result.error = source.getFileName() + " is not a song file, or it is damaged";
        return result;
    }

    if (auto info = juce::parseXML (readEntry (zip, "info.xml")))
    {
        if (info->getIntAttribute ("format", 1) > formatVersion)
        {
            result.error = source.getFileName() + " was saved by a newer version (" + info->getStringAttribute ("app")
                            + "). Update this app to open it.";
            return result;
        }
    }

    result.song = juce::ValueTree::fromXml (readEntry (zip, "song.xml"));

    if (! result.song.isValid() || ! result.song.hasType ("EDIT"))
    {
        result.song = {};
        result.error = source.getFileName() + " has no readable song in it";
        return result;
    }

    // Audio files: into this PC's asset store, each checked against its fingerprint.
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry (i);
        const auto name = entry->filename;

        if (! name.startsWith ("audio/") || ! name.endsWith (".flac"))
            continue;

        const auto hash = name.fromFirstOccurrenceOf ("audio/", false, false).upToLastOccurrenceOf (".flac", false, false);

        if (assets.has (hash))
        {
            ++result.audioAlreadyHere;
            continue;
        }

        auto temp = assets.getFolder().getNonexistentChildFile ("restore", ".tmp", false);
        bool written = false;

        if (std::unique_ptr<juce::InputStream> in { zip.createStreamForEntry (i) })
        {
            juce::FileOutputStream out (temp);

            if (! out.failedToOpen())
            {
                out.writeFromInputStream (*in, -1);
                out.flush();
                written = ! out.getStatus().failed();
            }
        }

        if (written && assets.adoptFile (temp, hash))
            ++result.audioRestored;
        else
        {
            temp.deleteFile();
            ++result.audioDamaged;
        }
    }

    return result;
}

} // namespace project

#include "ProjectFile.h"
#include "AppData.h"

namespace project
{

static juce::InputStream* streamOf (const juce::String& text)
{
    juce::MemoryBlock data (text.toRawUTF8(), text.getNumBytesAsUTF8());
    return new juce::MemoryInputStream (std::move (data), true);
}

static juce::String readEntry (juce::ZipFile& zip, const juce::String& name);

std::map<juce::String, juce::String> readAudioList (const juce::File& songFile)
{
    std::map<juce::String, juce::String> list;

    if (! songFile.existsAsFile())
        return list;

    juce::ZipFile zip (songFile);

    if (auto xml = juce::parseXML (readEntry (zip, "audio.xml")))
        for (auto* e : xml->getChildWithTagNameIterator ("FILE"))
            list[e->getStringAttribute ("path")] = e->getStringAttribute ("hash");

    return list;
}

/** Puts the song's audio into Audio\ and Takes\ next to the song file; returns the list for audio.xml. */
static std::unique_ptr<juce::XmlElement> writeAudioFolder (const juce::File& target, const AssetStore& assets,
                                                            const std::vector<AudioFile>& audio, SaveResult& result)
{
    auto list = std::make_unique<juce::XmlElement> ("AUDIO");
    const auto folder = target.getParentDirectory();
    const auto before = readAudioList (target);   // what the last save put there
    std::set<juce::String> used;

    for (auto& a : audio)
    {
        if (a.hash.isEmpty())
            continue;

        // Readable name: "Kick Hard.wav" -> Audio\Kick Hard.flac, takes -> Takes\Vocals take 1.flac
        const auto base = a.name.upToLastOccurrenceOf (".", false, false);
        auto stem = juce::File::createLegalFileName (base.isNotEmpty() ? base : a.name).trim();

        if (stem.isEmpty())
            stem = "Sound";

        const juce::String sub (a.isTake ? "Takes" : "Audio");
        juce::String path;

        for (int n = 1;; ++n)
        {
            path = sub + "/" + stem + (n > 1 ? " (" + juce::String (n) + ")" : juce::String()) + ".flac";

            if (used.count (path) > 0)
                continue;   // another sound of this song has that name

            const auto file = folder.getChildFile (path);

            if (! file.existsAsFile())
                break;

            // Already there: this sound (from the last save, or checked), or something else (try another name).
            auto it = before.find (path);

            if ((it != before.end() && it->second == a.hash) || AssetStore::fingerprint (file) == a.hash)
                break;
        }

        const auto file = folder.getChildFile (path);
        const bool present = file.existsAsFile();

        if (! present)
        {
            if (! assets.has (a.hash))
            {
                ++result.audioMissing;   // still arriving on this PC
                continue;
            }

            if (! appdata::linkOrCopy (assets.getFile (a.hash), file))
            {
                result.error = "Can't write " + file.getFullPathName() + " (is the disk full?)";
                return list;
            }

            ++result.audioWritten;
        }

        used.insert (path);
        ++result.audioIncluded;
        auto* e = list->createNewChildElement ("FILE");
        e->setAttribute ("hash", a.hash);
        e->setAttribute ("path", path);
        e->setAttribute ("name", a.name);
    }

    // Files an earlier save put there that the song doesn't use any more: to the recycle bin.
    for (auto& [path, hash] : before)
        if (used.count (path) == 0)
            folder.getChildFile (path).moveToTrash();

    return list;
}

SaveResult save (const juce::File& target, const juce::ValueTree& song, const AssetStore& assets,
                 const std::vector<AudioFile>& audio, bool includeAudio, const juce::String& savedBy)
{
    SaveResult result;

    if (! song.isValid())
    {
        result.error = "There is no song to save";
        return result;
    }

    juce::ZipFile::Builder zip;
    const auto now = juce::Time::getCurrentTime();

    juce::XmlElement info ("COTOSONG");
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
        target.getParentDirectory().createDirectory();
        auto list = writeAudioFolder (target, assets, audio, result);

        if (! result.ok())
            return result;

        zip.addEntry (streamOf (list->toString()), 9, "audio.xml", now);
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

    // Audio next to the song (Audio\, Takes\): into this PC's asset store, each checked.
    if (auto list = juce::parseXML (readEntry (zip, "audio.xml")))
    {
        for (auto* e : list->getChildWithTagNameIterator ("FILE"))
        {
            const auto hash = e->getStringAttribute ("hash");
            const auto file = source.getParentDirectory().getChildFile (e->getStringAttribute ("path"));

            if (assets.has (hash))
            {
                ++result.audioAlreadyHere;
                continue;
            }

            if (! file.existsAsFile())
            {
                ++result.audioMissing;
                continue;
            }

            auto temp = assets.getFolder().getNonexistentChildFile ("restore", ".tmp", false);

            if (appdata::linkOrCopy (file, temp) && assets.adoptFile (temp, hash))
                ++result.audioRestored;
            else
            {
                temp.deleteFile();
                ++result.audioDamaged;   // changed since it was saved: the song keeps asking for the original
            }
        }
    }

    // Older song files: the audio is inside the file.
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

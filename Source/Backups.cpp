#include "Backups.h"
#include "AppData.h"
#include "ProjectFile.h"

namespace backups
{

static juce::String stamp (juce::Time t)   { return t.formatted ("%Y-%m-%d %H-%M-%S"); }

juce::File folderForUnsaved (const juce::String& songTitle, juce::Time started)
{
    auto name = juce::File::createLegalFileName (songTitle.trim());

    if (name.isEmpty())
        name = "Untitled";

    return appdata::backupsFolder().getChildFile (name + " (started " + stamp (started) + ")");
}

juce::File newBackupFile (const juce::File& folder, const juce::String& songTitle)
{
    auto name = juce::File::createLegalFileName (songTitle.trim());

    if (name.isEmpty())
        name = "Untitled";

    return folder.getChildFile (name + " (backup " + stamp (juce::Time::getCurrentTime()) + ")" + appdata::songExtension);
}

static juce::Array<juce::File> backupsIn (const juce::File& folder)
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, appdata::songWildcards);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() > b.getLastModificationTime(); });
    return files;
}

std::map<juce::String, juce::String> knownAudio (const juce::File& folder)
{
    std::map<juce::String, juce::String> known;

    for (auto& f : backupsIn (folder))
        for (auto& [path, hash] : project::readAudioList (f))
            if (folder.getChildFile (path).existsAsFile())
                known.emplace (path, hash);

    return known;
}

void prune (const juce::File& folder, int keep)
{
    auto files = backupsIn (folder);

    for (int i = keep; i < files.size(); ++i)
        files.getReference (i).deleteFile();

    files.removeRange (keep, files.size());

    // Audio that none of the kept backups uses goes too.
    std::set<juce::String> used;

    for (auto& f : files)
        for (auto& [path, hash] : project::readAudioList (f))
            used.insert (folder.getChildFile (path).getFullPathName());

    for (auto* sub : { "Audio", "Takes" })
        for (auto& f : folder.getChildFile (sub).findChildFiles (juce::File::findFiles, false))
            if (used.count (f.getFullPathName()) == 0)
                f.deleteFile();
}

void pruneOldUnsaved (int days)
{
    const auto limit = juce::Time::getCurrentTime() - juce::RelativeTime::days (days);

    for (auto& folder : appdata::backupsFolder().findChildFiles (juce::File::findDirectories, false))
    {
        if (! folder.getFileName().contains (" (started "))
            continue;   // not ours

        bool recent = false;

        for (auto& f : backupsIn (folder))
            recent = recent || f.getLastModificationTime() > limit;

        if (! recent)
            folder.deleteRecursively();
    }
}

static Entry entryFor (const juce::File& f, bool thisSong)
{
    auto name = f.getFileNameWithoutExtension();
    return { f, name.contains (" (backup ") ? name.upToLastOccurrenceOf (" (backup ", false, false) : name,
             f.getLastModificationTime(), thisSong };
}

juce::Array<Entry> list (const juce::File& currentFolder)
{
    juce::Array<Entry> result;

    for (auto& f : backupsIn (currentFolder))
        result.add (entryFor (f, true));

    // Never-saved songs: their folders, and loose files from before 1.8.
    juce::Array<Entry> others;
    juce::Array<juce::File> folders { appdata::backupsFolder() };
    folders.addArray (appdata::backupsFolder().findChildFiles (juce::File::findDirectories, false));

    for (auto& folder : folders)
        if (folder != currentFolder)
            for (auto& f : backupsIn (folder))
                others.add (entryFor (f, false));

    std::sort (others.begin(), others.end(), [] (const Entry& a, const Entry& b) { return a.when > b.when; });
    result.addArray (others);
    return result;
}

juce::String describeWhen (juce::Time t)
{
    const auto now = juce::Time::getCurrentTime();
    const auto ago = now - t;

    if (ago.inMinutes() < 1.0)   return TRANS ("just now");
    if (ago.inMinutes() < 60.0)  return TRANS ("{N} min ago").replace ("{N}", juce::String ((int) ago.inMinutes()));

    const bool today = t.getDayOfMonth() == now.getDayOfMonth() && t.getMonth() == now.getMonth() && t.getYear() == now.getYear();
    const auto yesterdayTime = now - juce::RelativeTime::days (1);
    const bool yesterday = t.getDayOfMonth() == yesterdayTime.getDayOfMonth() && t.getMonth() == yesterdayTime.getMonth()
                            && t.getYear() == yesterdayTime.getYear();

    return (today ? TRANS ("today") : yesterday ? TRANS ("yesterday") : t.formatted ("%d/%m/%Y")) + " " + t.formatted ("%H:%M");
}

}

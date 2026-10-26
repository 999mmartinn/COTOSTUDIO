#include "AppData.h"

#if JUCE_WINDOWS
 #include "HardLinkWin.h"
#else
 #include <unistd.h>
#endif

namespace appdata
{

//==============================================================================
static juce::File& rootStorage()
{
    static juce::File root;
    return root;
}

static juce::File pointerFile()
{
    // Only written if you pick another data folder: the one thing COTOSTUDIO keeps on C:.
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile (appName).getChildFile ("data-folder.txt");
}

juce::File defaultRoot()
{
   #if JUCE_WINDOWS
    const juce::File onD ("D:\\COTOSTUDIO");

    if (onD.isDirectory())
        return onD;
   #endif

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName);
}

juce::File getRoot()
{
    auto& root = rootStorage();

    if (root == juce::File())
    {
        const auto chosen = pointerFile().loadFileAsString().trim();

        if (chosen.isNotEmpty() && juce::File::isAbsolutePath (chosen) && juce::File (chosen).createDirectory())
            root = juce::File (chosen);
        else
            root = defaultRoot();

        root.createDirectory();
    }

    return root;
}

void chooseRoot (const juce::File& folder)
{
    pointerFile().getParentDirectory().createDirectory();
    pointerFile().replaceWithText (folder.getFullPathName());
}

void setRootForThisRun (const juce::File& folder)
{
    rootStorage() = folder;
    folder.createDirectory();
}

static juce::File sub (const char* name)
{
    auto f = getRoot().getChildFile (name);
    f.createDirectory();
    return f;
}

juce::File projectsFolder()     { return sub ("Projects"); }
juce::File templatesFolder()    { return sub ("Templates"); }
juce::File backupsFolder()      { return sub ("Backups"); }
juce::File exportsFolder()      { return sub ("Exports"); }
juce::File settingsFolder()     { return sub ("Settings"); }
juce::File cacheFolder()        { return sub ("Cache"); }

juce::File audioCacheFolder (int copyNumber)
{
    return cacheFolder().getChildFile (copyNumber <= 1 ? juce::String ("Audio") : "Audio-" + juce::String (copyNumber));
}

juce::File songFileForName (const juce::String& name)
{
    const auto legal = juce::File::createLegalFileName (name.trim()).trim();
    const auto stem = legal.isNotEmpty() ? legal : juce::String ("Untitled");
    return projectsFolder().getChildFile (stem).getChildFile (stem + songExtension);
}

juce::File templateFileForName (const juce::String& name)
{
    const auto legal = juce::File::createLegalFileName (name.trim()).trim();
    const auto stem = legal.isNotEmpty() ? legal : juce::String ("Template");
    return templatesFolder().getChildFile (stem).getChildFile (stem + songExtension);
}

juce::Array<juce::File> findTemplates()
{
    juce::Array<juce::File> list;

    for (auto& folder : templatesFolder().findChildFiles (juce::File::findDirectories, false))
    {
        auto f = folder.getChildFile (folder.getFileName() + songExtension);

        if (f.existsAsFile())
            list.add (f);
    }

    std::sort (list.begin(), list.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });
    return list;
}

juce::File projectSongFile (const juce::File& chosen)
{
    const auto name = chosen.getFileNameWithoutExtension();
    const auto parent = chosen.getParentDirectory();

    if (parent.getFileName() == name)
        return parent.getChildFile (name + songExtension);

    return parent.getChildFile (name).getChildFile (name + songExtension);
}

bool isBackup (const juce::File& songFile)
{
    return songFile.getParentDirectory().getFileName() == "Backups";
}

bool linkOrCopy (const juce::File& source, const juce::File& target)
{
    if (! source.existsAsFile())
        return false;

    target.getParentDirectory().createDirectory();
    target.deleteFile();

   #if JUCE_WINDOWS
    if (hardlink::create (target.getFullPathName().toWideCharPointer(), source.getFullPathName().toWideCharPointer()))
        return true;
   #else
    if (::link (source.getFullPathName().toRawUTF8(), target.getFullPathName().toRawUTF8()) == 0)
        return true;
   #endif

    return source.copyFileTo (target);
}

//==============================================================================
// Moving the old data off C:
juce::File oldAppDataFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SyncTest");
}

juce::File oldSongsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("SyncTest Songs");
}

static juce::File freeName (const juce::File& wanted, const std::set<juce::String>& taken)
{
    // "Name.ext", "Name (2).ext"... not on disk and not planned yet.
    auto target = wanted;

    for (int n = 2; target.exists() || taken.count (target.getFullPathName()) > 0; ++n)
        target = wanted.getSiblingFile (wanted.getFileNameWithoutExtension() + " (" + juce::String (n) + ")"
                                         + wanted.getFileExtension());

    return target;
}

Migration planMigration (const juce::File& oldAppData, const juce::File& oldSongs, const juce::File& newRoot)
{
    Migration m;
    std::set<juce::String> taken;

    auto add = [&] (const juce::File& from, juce::File to, bool renameIfTaken)
    {
        if (renameIfTaken)
            to = freeName (to, taken);
        else if (to.exists() || taken.count (to.getFullPathName()) > 0)
            to = juce::File();   // already there (same content: audio files are named by their fingerprint)

        taken.insert (to.getFullPathName());
        m.moves.push_back ({ from, to, false });
        m.totalBytes += from.getSize();
        return to;
    };

    if (oldAppData.isDirectory())
    {
        const auto settings = newRoot.getChildFile ("Settings");

        // Your settings and the engine's (plugin list, audio device) - unless the new place has them already.
        const auto oldSettings = oldAppData.getChildFile ("SyncTest.settings");

        if (oldSettings.existsAsFile() && ! settings.getChildFile ("COTOSTUDIO.settings").exists())
            add (oldSettings, settings.getChildFile ("COTOSTUDIO.settings"), false);

        const auto oldEngine = oldAppData.getChildFile ("Settings.xml");

        if (oldEngine.existsAsFile() && ! settings.getChildFile ("Settings.xml").exists())
            add (oldEngine, settings.getChildFile ("Settings.xml"), false);

        // The shared audio of each copy of the app: Assets -> Cache\Audio, Assets-2 -> Cache\Audio-2...
        for (auto& folder : oldAppData.findChildFiles (juce::File::findDirectories, false, "Assets*"))
        {
            const auto name = folder.getFileName();

            if (name.endsWith ("-recordings"))
                continue;   // raw takes already turned into shared files: dropped

            const auto suffix = name.fromFirstOccurrenceOf ("Assets", false, false);   // "" or "-2"...
            const auto target = newRoot.getChildFile ("Cache").getChildFile ("Audio" + suffix);

            for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.flac"))
                add (f, target.getChildFile (f.getFileName()), false);
        }

        // Automatic backups
        for (auto& f : oldAppData.getChildFile ("Autosave").findChildFiles (juce::File::findFiles, false, "*.syncsong"))
            add (f, newRoot.getChildFile ("Backups").getChildFile (f.getFileName()), true);

        m.toDeleteAfterwards.add (oldAppData);
    }

    if (oldSongs.isDirectory())
    {
        // Each song gets a project folder of its own.
        for (auto& f : oldSongs.findChildFiles (juce::File::findFiles, false, "*.syncsong"))
        {
            const auto stem = f.getFileNameWithoutExtension();
            auto target = newRoot.getChildFile ("Projects").getChildFile (stem).getChildFile (f.getFileName());
            target = add (f, target, true);
            m.renamedSongs[f.getFullPathName()] = target.getFullPathName();
        }

        // Exports keep their folders (stems).
        const auto oldExports = oldSongs.getChildFile ("Exports");

        for (auto& f : oldExports.findChildFiles (juce::File::findFiles, true))
            add (f, newRoot.getChildFile ("Exports").getChildFile (f.getRelativePathFrom (oldExports)), true);

        m.toDeleteAfterwards.add (oldSongs);   // only if nothing of yours is left in it
    }

    return m;
}

static bool isEmptyFolderTree (const juce::File& folder)
{
    return folder.findChildFiles (juce::File::findFiles, true).isEmpty();
}

MigrationResult runMigration (const Migration& m, std::function<void (double)> progress)
{
    MigrationResult r;
    juce::int64 done = 0;

    for (auto& mv : m.moves)
    {
        const auto size = mv.from.getSize();

        if (mv.to == juce::File())
        {
            mv.from.deleteFile();   // the same file is already in the new place
        }
        else
        {
            mv.to.getParentDirectory().createDirectory();

            // Copy, check, then remove the original (works across drives).
            if (mv.from.copyFileTo (mv.to) && mv.to.getSize() == size)
            {
                mv.from.deleteFile();
                ++r.filesMoved;
                r.bytesMoved += size;
            }
            else
            {
                mv.to.deleteFile();
                ++r.failures;
                r.problems.add ("Couldn't move " + mv.from.getFullPathName());
            }
        }

        done += size;

        if (progress)
            progress (m.totalBytes > 0 ? (double) done / (double) m.totalBytes : 1.0);
    }

    // "Recent songs" in your settings now point at the songs' new places.
    for (auto& mv : m.moves)
    {
        if (mv.to.getFileName() != "COTOSTUDIO.settings" || ! mv.to.existsAsFile())
            continue;

        juce::PropertiesFile settings (mv.to, juce::PropertiesFile::Options());
        auto recent = juce::StringArray::fromLines (settings.getValue ("recentSongs"));

        for (auto& path : recent)
            if (auto it = m.renamedSongs.find (path); it != m.renamedSongs.end())
                path = it->second;

        recent.removeEmptyStrings();
        settings.setValue ("recentSongs", recent.joinIntoString ("\n"));
        settings.saveIfNeeded();
    }

    // The old folders go once nothing of yours is left in them (their leftovers are temporary files).
    if (r.failures == 0)
        for (auto& folder : m.toDeleteAfterwards)
            if (folder.isDirectory()
                 && (folder.getFileName() == "SyncTest" || isEmptyFolderTree (folder)))
                folder.deleteRecursively();

    if (progress)
        progress (1.0);

    return r;
}

} // namespace appdata

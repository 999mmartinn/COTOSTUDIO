#include "MainComponent.h"
#include "PluginWindow.h"
#include "SelfTest.h"
#include "DrumSynthPlugin.h"
#include "SamplerPlugin.h"
#include "ParametricEQPlugin.h"
#include "LimiterPlugin.h"
#include "DelayPlugin.h"
#include "ReverbPlugin.h"
#include "AppData.h"
#include "Recorder.h"
#include "Theme.h"
#include "BinaryData.h"
#include "FlpImportDialog.h"
#include <iostream>

/** Engine settings: raw recordings go to this copy's own folder (see Recorder). */
struct AppEngineBehaviour  : public te::EngineBehaviour
{
    juce::File getFileForNewAudioRecording (te::Track& track, const juce::String& extension) override
    {
        return Recorder::newRecordingFile (track.edit, extension);
    }

    /** The engine's defaults allow only 4 plugins on the master and 16 on a track. A mixer
        strip has 10 effect slots, plus volume, meter, one plugin per send and the returns,
        so give plenty of room (more than this never happens through the interface). */
    te::EditLimits getEditLimits() override
    {
        te::EditLimits limits;
        limits.maxPluginsOnTrack = 64;
        limits.maxNumMasterPlugins = 64;
        return limits;
    }
};

/** The engine's own settings (plugin list, audio device) and temporary files live in the
    data folder too, not on C:. */
struct AppPropertyStorage  : public te::PropertyStorage
{
    AppPropertyStorage() : te::PropertyStorage (appdata::appName) {}

    juce::File getAppPrefsFolder() override         { return appdata::settingsFolder(); }
    juce::String getApplicationVersion() override   { return ProjectInfo::versionString; }

    juce::File getAppCacheFolder() override
    {
        auto f = appdata::cacheFolder().getChildFile ("Engine");
        f.createDirectory();
        return f;
    }
};

//==============================================================================
/** Shown once, while the files of an older version (SyncTest) move off C: into the data folder. */
class MigrationWindow  : public juce::DocumentWindow,
                         private juce::Timer
{
public:
    MigrationWindow (const appdata::Migration& plan, std::function<void (appdata::MigrationResult)> done)
        : DocumentWindow (appdata::appName, juce::Colour (0xff14111c), 0), onDone (std::move (done))
    {
        setUsingNativeTitleBar (true);
        content.text = "Moving your songs and files to " + appdata::getRoot().getFullPathName()
                        + "\n(" + juce::File::descriptionOfSizeInBytes (plan.totalBytes) + ", only this once)";
        content.setSize (460, 140);
        setContentNonOwned (&content, true);
        centreWithSize (460, 140);
        setVisible (true);

        worker = std::thread ([this, plan]
        {
            result = appdata::runMigration (plan, [this] (double p) { progress = p; });
            finished = true;
        });

        startTimerHz (10);
    }

    ~MigrationWindow() override
    {
        stopTimer();

        if (worker.joinable())
            worker.join();
    }

private:
    struct Content  : public juce::Component
    {
        juce::String text;
        double shownProgress = 0.0;

        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::colours::panel);
            g.setColour (theme::colours::text);
            g.setFont (theme::ui (14.0f, theme::Weight::medium));
            g.drawFittedText (text, getLocalBounds().reduced (20).removeFromTop (60), juce::Justification::centredLeft, 3);

            auto bar = getLocalBounds().reduced (20).removeFromBottom (14).toFloat();
            g.setColour (theme::colours::stepOff);
            g.fillRoundedRectangle (bar, 7.0f);
            g.setColour (theme::colours::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) shownProgress), 7.0f);
        }
    };

    Content content;
    std::function<void (appdata::MigrationResult)> onDone;
    std::thread worker;
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> finished { false };
    appdata::MigrationResult result;

    void timerCallback() override
    {
        content.shownProgress = progress.load();
        content.repaint();

        if (finished)
        {
            stopTimer();
            worker.join();
            auto callback = onDone;
            const auto r = result;
            callback (r);   // (deletes this window)
        }
    }

    void closeButtonPressed() override {}
};

//==============================================================================
class CotoStudioApplication  : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override             { return true; }   // two copies on one PC for a quick test

    void initialise (const juce::String&) override
    {
        auto args = getCommandLineParameterArray();

        for (auto& a : args)
            if (a.startsWith ("--data-folder="))   // (for testing: a separate data folder)
                appdata::setRootForThisRun (juce::File (a.fromFirstOccurrenceOf ("=", false, false).unquoted()));

        // The look (Nocturno) for every window, and the language you chose (English unless you picked Spanish).
        juce::LookAndFeel::setDefaultLookAndFeel (&theme::look());

        if (! args.contains ("--selftest"))
            applyLanguage (args);

        for (auto& a : args)
        {
            if (a.startsWith ("--flp-info="))   // (developer: what COTOSTUDIO reads from an FL Studio project)
            {
                const juce::File flp (a.fromFirstOccurrenceOf ("=", false, false).unquoted());
                std::cout << flimport::describeFile (flp) << std::endl;

                // --flp-dry-run [--flp-skip=Name|Name] [--flp-skip-off]: what the import would build
                // here, with this PC's plugin list (nothing is saved).
                if (args.contains ("--flp-dry-run"))
                {
                    createEngine();
                    std::cout << "\n=== dry run ===\n" << flimport::dryRun (*engine, flp, args) << std::endl;
                }

                // --flp-dialog-image=<png>: a picture of the import dialog for this file (checking its layout).
                for (auto& b : args)
                {
                    if (b.startsWith ("--flp-dialog-image="))
                    {
                        flimport::Project project;

                        if (flimport::parse (flp, project).isEmpty())
                        {
                            FlpImportDialog dialog (project, flp.getFileName(), {});
                            const juce::File png (b.fromFirstOccurrenceOf ("=", false, false).unquoted());
                            png.deleteFile();
                            juce::FileOutputStream out (png);
                            juce::PNGImageFormat().writeImageToStream (dialog.createComponentSnapshot (dialog.getLocalBounds(), true, 1.0f), out);
                        }
                    }
                }

                setApplicationReturnValue (0);
                quit();
                return;
            }
        }

        if (args.contains ("--selftest"))
        {
            // The self-test never touches your data folder.
            appdata::setRootForThisRun (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("cotostudio-selftest-data"));
            createEngine();
            selfTest = std::make_unique<SelfTest> (*engine, args, [this] (int code)
            {
                setApplicationReturnValue (code);
                quit();
            });
            return;
        }

        // First start after an older version (SyncTest): move its files off C: first.
        const auto plan = appdata::planMigration (appdata::oldAppDataFolder(), appdata::oldSongsFolder(), appdata::getRoot());

        if (plan.isNeeded())
        {
            migrationWindow = std::make_unique<MigrationWindow> (plan, [this] (appdata::MigrationResult r)
            {
                juce::MessageManager::callAsync ([this, r]
                {
                    migrationWindow.reset();
                    start();

                    if (! r.problems.isEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Some files stayed where they were"),
                                                                r.problems.joinIntoString ("\n").substring (0, 2000));
                });
            });
            return;
        }

        start();
    }

    void shutdown() override
    {
        migrationWindow.reset();
        selfTest.reset();
        mainWindow.reset();
        engine.reset();

        // Unsaved audio of the song just closed that was still open a moment ago (Windows
        // can't delete open files): the engine has let go of it now.
        for (auto& folder : appdata::cacheFolder().findChildFiles (juce::File::findDirectories, false, "Audio*"))
            AssetStore::retryPendingDeletes (folder);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        juce::LocalisedStrings::setCurrentMappings (nullptr);
    }

    void systemRequestedQuit() override
    {
        if (migrationWindow != nullptr)
            return;   // let it finish

        // Ask to save changes first (the main window decides when to really quit).
        if (mainWindow != nullptr)
            if (auto* content = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            {
                content->requestQuit();
                return;
            }

        quit();
    }

private:
    class MainWindow  : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (te::Engine& e)
            : DocumentWindow (appdata::appName, juce::Colour (0xff14111c), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent (e);
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (1240, 700, 8000, 5000);
            settings = &content->getSettings();

            // Where you left it; the first time, filling the screen.
            const auto saved = settings->getValue ("mainWindow");

            if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
            {
                centreWithSize (getWidth(), getHeight());
                setFullScreen (true);   // (on Windows: maximised)
            }

            setVisible (true);
            ready = true;
        }

        void closeButtonPressed() override                 { remember(); juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        void moved() override                              { DocumentWindow::moved(); remember(); }
        void resized() override                            { DocumentWindow::resized(); remember(); }

    private:
        juce::PropertiesFile* settings = nullptr;
        bool ready = false;

        void remember()
        {
            if (ready && settings != nullptr && ! isMinimised())
                settings->setValue ("mainWindow", getWindowStateAsString());
        }
    };

    std::unique_ptr<juce::LocalisedStrings> translation;

    /** Spanish if you picked it in Settings > Language (or --lang=es), otherwise English. */
    void applyLanguage (const juce::StringArray& args)
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName = appdata::appName;
        juce::PropertiesFile prefs (appdata::settingsFolder().getChildFile ("COTOSTUDIO.settings"), opts);
        auto code = prefs.getValue ("language", "en");

        for (auto& a : args)
            if (a.startsWith ("--lang="))
                code = a.fromFirstOccurrenceOf ("=", false, false).trim();

        if (code != "es")
            return;

        int size = 0;

        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            if (juce::String (BinaryData::originalFilenames[i]) == "es.txt")
                if (auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
                {
                    // ignoreCase = false: "Mixer" and "mixer" can differ.
                    juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (juce::String::fromUTF8 (data, size), false));
                    break;
                }
    }

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<SelfTest> selfTest;
    std::unique_ptr<MigrationWindow> migrationWindow;

    void createEngine()
    {
        engine = std::make_unique<te::Engine> (std::make_unique<AppPropertyStorage>(), std::make_unique<AppUIBehaviour>(),
                                               std::make_unique<AppEngineBehaviour>());
        engine->getPluginManager().createBuiltInType<DrumSynthPlugin>();
        engine->getPluginManager().createBuiltInType<SamplerPlugin>();
        engine->getPluginManager().createBuiltInType<ParametricEQPlugin>();
        engine->getPluginManager().createBuiltInType<LimiterPlugin>();
        engine->getPluginManager().createBuiltInType<DelayPlugin>();
        engine->getPluginManager().createBuiltInType<ReverbPlugin>();

        // One input per interface socket (a mic is mono): "Input 1", "Input 2"...
        engine->getDeviceManager().setAllWaveInputsToNumChannels (1);
    }

    void start()
    {
        createEngine();
        mainWindow = std::make_unique<MainWindow> (*engine);
    }
};

START_JUCE_APPLICATION (CotoStudioApplication)

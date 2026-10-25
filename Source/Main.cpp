#include "MainComponent.h"
#include "PluginWindow.h"
#include "SelfTest.h"
#include "DrumSynthPlugin.h"
#include "SamplerPlugin.h"
#include "AppData.h"
#include "Recorder.h"

/** Engine settings: raw recordings go to this copy's own folder (see Recorder). */
struct AppEngineBehaviour  : public te::EngineBehaviour
{
    juce::File getFileForNewAudioRecording (te::Track& track, const juce::String& extension) override
    {
        return Recorder::newRecordingFile (track.edit, extension);
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
            g.fillAll (juce::Colour (0xff1f1b2a));
            g.setColour (juce::Colour (0xffeeeaf8));
            g.setFont (juce::FontOptions (15.0f));
            g.drawFittedText (text, getLocalBounds().reduced (20).removeFromTop (60), juce::Justification::centredLeft, 3);

            auto bar = getLocalBounds().reduced (20).removeFromBottom (14).toFloat();
            g.setColour (juce::Colour (0xff2b2639));
            g.fillRoundedRectangle (bar, 7.0f);
            g.setColour (juce::Colour (0xffb39bff));
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
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Some files stayed where they were",
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
            setContentOwned (new MainComponent (e), true);
            setResizable (true, true);
            setResizeLimits (1000, 640, 8000, 5000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override                 { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

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

#include "MainComponent.h"
#include "PluginWindow.h"
#include "SelfTest.h"
#include "DrumSynthPlugin.h"

#include "Recorder.h"

/** Engine settings: raw recordings go to this copy's own folder (see Recorder). */
struct AppEngineBehaviour  : public te::EngineBehaviour
{
    juce::File getFileForNewAudioRecording (te::Track& track, const juce::String& extension) override
    {
        return Recorder::newRecordingFile (track.edit, extension);
    }
};

class SyncTestApplication  : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override             { return true; }   // two copies on one PC for a quick test

    void initialise (const juce::String&) override
    {
        engine = std::make_unique<te::Engine> ("SyncTest", std::make_unique<AppUIBehaviour>(),
                                               std::make_unique<AppEngineBehaviour>());
        engine->getPluginManager().createBuiltInType<DrumSynthPlugin>();

        // One input per interface socket (a mic is mono): "Input 1", "Input 2"...
        engine->getDeviceManager().setAllWaveInputsToNumChannels (1);

        auto args = getCommandLineParameterArray();

        if (args.contains ("--selftest"))
        {
            selfTest = std::make_unique<SelfTest> (*engine, args, [this] (int code)
            {
                setApplicationReturnValue (code);
                quit();
            });
            return;
        }

        mainWindow = std::make_unique<MainWindow> (*engine);
    }

    void shutdown() override
    {
        selfTest.reset();
        mainWindow.reset();
        engine.reset();
    }

    void systemRequestedQuit() override                    { quit(); }

private:
    class MainWindow  : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (te::Engine& e)
            : DocumentWindow ("SyncTest - multiplayer DAW", juce::Colour (0xff1e2227), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (e), true);
            setResizable (true, true);
            setResizeLimits (900, 600, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override                 { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<SelfTest> selfTest;
};

START_JUCE_APPLICATION (SyncTestApplication)

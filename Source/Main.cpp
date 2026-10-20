#include "MainComponent.h"
#include "PluginWindow.h"
#include "SelfTest.h"

class SyncTestApplication  : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion() override    { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override             { return true; }   // two copies on one PC for a quick test

    void initialise (const juce::String&) override
    {
        engine = std::make_unique<te::Engine> ("SyncTest", std::make_unique<AppUIBehaviour>(), nullptr);

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
            : DocumentWindow ("SyncTest - two-peer sync test", juce::Colour (0xff1e2227), DocumentWindow::allButtons)
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

#include "MainComponent.h"
#include "SongHelpers.h"

static const juce::Colour background (0xff1e2227), panel (0xff2a2f36), accent (0xfff28c28), cellOff (0xff3a414a);

//==============================================================================
juce::Rectangle<float> StepGrid::cellBounds (int step, int row) const
{
    auto w = (float) getWidth() / (float) song::numSteps;
    auto h = (float) getHeight() / (float) numRows;
    return { step * w, row * h, w, h };
}

void StepGrid::paint (juce::Graphics& g)
{
    g.fillAll (panel);
    auto* edit = getEdit ? getEdit() : nullptr;
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    for (int row = 0; row < numRows; ++row)
    {
        const int pitch = lowestPitch + (numRows - 1 - row);

        for (int step = 0; step < song::numSteps; ++step)
        {
            auto r = cellBounds (step, row).reduced (2.0f);
            const bool on = edit != nullptr && song::hasNote (*edit, trackIndex, pitch, step);
            auto off = (step / 4) % 2 == 0 ? cellOff : cellOff.brighter (0.12f);
            g.setColour (on ? accent : off);
            g.fillRoundedRectangle (r, 3.0f);

            if (step == 0)
            {
                g.setColour (on ? juce::Colours::black : juce::Colours::white.withAlpha (0.5f));
                g.setFont (11.0f);
                g.drawText (names[pitch % 12], r.reduced (3.0f, 0.0f), juce::Justification::centredLeft);
            }
        }
    }
}

void StepGrid::mouseDown (const juce::MouseEvent& e)
{
    auto* edit = getEdit ? getEdit() : nullptr;

    if (edit == nullptr)
        return;

    const int step = juce::jlimit (0, song::numSteps - 1, (int) (e.position.x / getWidth() * song::numSteps));
    const int row  = juce::jlimit (0, numRows - 1, (int) (e.position.y / getHeight() * numRows));
    const int pitch = lowestPitch + (numRows - 1 - row);

    song::setNote (*edit, trackIndex, pitch, step, ! song::hasNote (*edit, trackIndex, pitch, step));
    repaint();
}

//==============================================================================
MainComponent::MainComponent (te::Engine& e) : engine (e)
{
    juce::PropertiesFile::Options opts;
    opts.applicationName = "SyncTest";
    opts.filenameSuffix = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    appProperties.setStorageParameters (opts);

    auto* settings = appProperties.getUserSettings();
    portEditor.setText (settings->getValue ("port", "47800"));
    addressEditor.setText (settings->getValue ("address", "127.0.0.1"));
    portEditor.setInputRestrictions (5, "0123456789");

    for (auto* c : std::initializer_list<juce::Component*> { &hostButton, &joinButton, &leaveButton, &portEditor, &addressEditor,
                                                             &portLabel, &addressLabel, &statusLabel, &playButton,
                                                             &addTrackButton, &addPluginButton, &scanButton, &audioButton,
                                                             &trackCaption, &gridCaption, &trackPanel, &grid, &logView })
        addAndMakeVisible (c);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setFont (juce::FontOptions (13.0f));
    statusLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));

    for (auto* l : { &trackCaption, &gridCaption })
    {
        l->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        l->setFont (juce::FontOptions (13.0f));
        l->setBorderSize ({});
    }

    grid.getEdit = [this] { return session.getEdit(); };

    session.onLog = [this] (const juce::String& s) { appendLog (s); };
    session.onStatusChanged = [this] { refresh(); };
    session.onRemoteChange = [this] { triggerAsyncUpdate(); };
    session.onEditAboutToBeReplaced = [this] { unwatchEdit(); };
    session.onEditReplaced = [this] { watchEdit(); refresh(); };

    hostButton.onClick = [this]
    {
        appProperties.getUserSettings()->setValue ("port", portEditor.getText());
        session.startHosting (portEditor.getText().getIntValue());
    };

    joinButton.onClick = [this]
    {
        appProperties.getUserSettings()->setValue ("port", portEditor.getText());
        appProperties.getUserSettings()->setValue ("address", addressEditor.getText());
        session.join (addressEditor.getText().trim(), portEditor.getText().getIntValue());
    };

    leaveButton.onClick = [this] { session.leave(); };

    playButton.onClick = [this]
    {
        if (auto* edit = session.getEdit())
            session.setPlaying (! edit->getTransport().isPlaying());

        refresh();
    };

    addTrackButton.onClick = [this]
    {
        if (auto* edit = session.getEdit())
        {
            song::addSynthTrack (*edit);
            selectedTrack = te::getAudioTracks (*edit).size() - 1;
        }
    };

    addPluginButton.onClick = [this] { showPluginMenu(); };
    scanButton.onClick = [this] { showPluginScanner(); };
    audioButton.onClick = [this] { showAudioSettings(); };

    session.createLocalEdit();
    appendLog ("SyncTest " + juce::JUCEApplication::getInstance()->getApplicationVersion()
                + ". First time: press 'Scan plugins...' so your VST3 plugins can be found.");
    setSize (1100, 720);
}

MainComponent::~MainComponent()
{
    unwatchEdit();
}

void MainComponent::appendLog (const juce::String& s)
{
    logView.moveCaretToEnd();
    logView.insertTextAtCaret (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + s + "\n");
}

void MainComponent::watchEdit()
{
    if (auto* edit = session.getEdit())
    {
        watchedState = edit->state;
        watchedState.addListener (this);
        selectedTrack = juce::jlimit (0, juce::jmax (0, te::getAudioTracks (*edit).size() - 1), selectedTrack);
    }
}

void MainComponent::unwatchEdit()
{
    watchedState.removeListener (this);
    watchedState = {};
    trackButtons.clear();
    pluginButtons.clear();
}

//==============================================================================
void MainComponent::refresh()
{
    using Mode = collab::Session::Mode;
    auto mode = session.getMode();

    statusLabel.setText (session.getStatusText(), juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId,
                           mode == Mode::offline ? juce::Colours::lightgrey
                                                 : (session.isInSync() ? juce::Colours::lightgreen : juce::Colours::orange));

    hostButton.setEnabled (mode == Mode::offline);
    joinButton.setEnabled (mode == Mode::offline);
    leaveButton.setEnabled (mode != Mode::offline);
    portEditor.setEnabled (mode == Mode::offline);
    addressEditor.setEnabled (mode == Mode::offline);

    if (auto* edit = session.getEdit())
        playButton.setButtonText (edit->getTransport().isPlaying() ? "Stop" : "Play");

    rebuildTrackPanel();
    grid.trackIndex = selectedTrack;
    grid.repaint();
}

void MainComponent::rebuildTrackPanel()
{
    trackButtons.clear();
    pluginButtons.clear();

    auto* edit = session.getEdit();

    if (edit == nullptr)
        return;

    auto tracks = te::getAudioTracks (*edit);

    for (int i = 0; i < tracks.size(); ++i)
    {
        auto* track = tracks[i];
        auto* b = trackButtons.add (new juce::TextButton (track->getName()));
        b->setClickingTogglesState (false);
        b->setToggleState (i == selectedTrack, juce::dontSendNotification);
        b->setColour (juce::TextButton::buttonOnColourId, accent.darker (0.3f));
        b->onClick = [this, i] { selectedTrack = i; refresh(); };
        b->getProperties().set ("row", i);
        trackPanel.addAndMakeVisible (b);

        for (auto* plugin : track->pluginList.getPlugins())
        {
            if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) != nullptr
                 || dynamic_cast<te::LevelMeterPlugin*> (plugin) != nullptr)
                continue;

            auto* ext = dynamic_cast<te::ExternalPlugin*> (plugin);
            const bool missing = ext != nullptr && ext->getAudioPluginInstance() == nullptr;
            auto name = plugin->getName() + (missing ? " (not installed here)" : "");

            auto* pb = pluginButtons.add (new juce::TextButton (name));
            pb->getProperties().set ("row", i);
            pb->setColour (juce::TextButton::buttonColourId, missing ? juce::Colours::darkred : panel.brighter (0.15f));
            te::Plugin::Ptr ptr (plugin);
            pb->onClick = [this, ptr]
            {
                if (juce::ModifierKeys::currentModifiers.isPopupMenu())
                    showPluginOptions (*ptr);
                else
                    ptr->showWindowExplicitly();
            };
            pb->setTooltip ("Click: open. Right-click (or Ctrl+click): options");
            trackPanel.addAndMakeVisible (pb);
        }
    }

    layoutTrackPanel();
}

void MainComponent::layoutTrackPanel()
{
    const int rowH = 30;
    std::map<int, int> x;

    for (auto* b : trackButtons)
    {
        int row = b->getProperties()["row"];
        b->setBounds (0, row * rowH + 2, 120, rowH - 4);
        x[row] = 126;
    }

    for (auto* b : pluginButtons)
    {
        int row = b->getProperties()["row"];
        int w = juce::jmin (220, juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (14.0f)), b->getButtonText()) + 24);
        b->setBounds (x[row], row * rowH + 2, w, rowH - 4);
        x[row] += w + 4;
    }
}

void MainComponent::showPluginOptions (te::Plugin& plugin)
{
    juce::PopupMenu m;
    m.addItem (1, "Open window");
    m.addItem (2, "Remove from track");
    te::Plugin::Ptr ptr (&plugin);

    m.showMenuAsync ({}, [ptr] (int r)
    {
        if (r == 1) ptr->showWindowExplicitly();
        if (r == 2) ptr->deleteFromParent();
    });
}

void MainComponent::showPluginMenu()
{
    auto* edit = session.getEdit();

    if (edit == nullptr)
        return;

    auto& list = engine.getPluginManager().knownPluginList;
    auto types = list.getTypes();

    if (types.isEmpty())
    {
        appendLog ("No plugins known yet. Press 'Scan plugins...' first.");
        return;
    }

    juce::PopupMenu m;
    juce::KnownPluginList::addToMenu (m, types, juce::KnownPluginList::sortByManufacturer);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addPluginButton),
                     [this, types] (int result)
    {
        auto index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

        if (! juce::isPositiveAndBelow (index, types.size()))
            return;

        if (auto* edit2 = session.getEdit())
            if (auto* track = song::getTrack (*edit2, selectedTrack))
                if (auto p = song::insertPlugin (*track, types[index]))
                    appendLog ("Added " + types[index].name + " to " + track->getName());
    });
}

void MainComponent::showPluginScanner()
{
    auto& pm = engine.getPluginManager();
    auto deadMansPedal = appProperties.getUserSettings()->getFile().getSiblingFile ("SyncTestScanCrashes.txt");

    auto* scanner = new juce::PluginListComponent (pm.pluginFormatManager, pm.knownPluginList, deadMansPedal,
                                                   appProperties.getUserSettings(), true);
    scanner->setSize (700, 500);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (scanner);
    o.dialogTitle = "Plugins - use Options > Scan for new or updated VST3 plug-ins";
    o.dialogBackgroundColour = background;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager().deviceManager,
                                                             0, 2, 0, 2, true, true, true, false);
    selector->setSize (500, 450);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio settings";
    o.dialogBackgroundColour = background;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.launchAsync();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (12);

    auto top = r.removeFromTop (30);
    hostButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (8);
    portLabel.setBounds (top.removeFromLeft (36));
    portEditor.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (16);
    addressLabel.setBounds (top.removeFromLeft (92));
    addressEditor.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (8);
    joinButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (8);
    leaveButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (16);
    statusLabel.setBounds (top);

    r.removeFromTop (10);
    auto row2 = r.removeFromTop (30);
    for (auto* b : { &playButton, &addTrackButton, &addPluginButton, &scanButton, &audioButton })
    {
        b->setBounds (row2.removeFromLeft (b == &scanButton || b == &audioButton ? 130 : 90));
        row2.removeFromLeft (8);
    }

    r.removeFromTop (8);
    trackCaption.setBounds (r.removeFromTop (22));
    trackPanel.setBounds (r.removeFromTop (150));
    r.removeFromTop (6);
    gridCaption.setBounds (r.removeFromTop (22));
    logView.setBounds (r.removeFromBottom (130));
    r.removeFromBottom (10);
    grid.setBounds (r);
    layoutTrackPanel();
}

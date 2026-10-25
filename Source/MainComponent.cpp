#include "MainComponent.h"
#include "PatternCompiler.h"

static const juce::Colour background (0xff1e2227), accent (0xfff28c28);

//==============================================================================
//==============================================================================
/** Developer option for testing without a sound card: SyncTest --simulate-mic.
    A fake interface (engine "hosted" device) with one input that sings a
    wobbling tone, fed in real time. */
struct MainComponent::SimulatedMic  : private juce::Timer
{
    explicit SimulatedMic (te::Engine& e) : engine (e), io (e.getDeviceManager().getHostedAudioDeviceInterface())
    {
        te::HostedAudioDeviceInterface::Parameters p;
        p.sampleRate = 44100.0;
        p.blockSize = 512;
        p.inputChannels = 1;
        p.outputChannels = 2;
        io.initialise (p);
        io.prepareToPlay (p.sampleRate, p.blockSize);
        engine.getDeviceManager().dispatchPendingUpdates();
        startTime = juce::Time::getMillisecondCounterHiRes();
        startTimer (10);
    }

    ~SimulatedMic() override
    {
        stopTimer();
        engine.getDeviceManager().deviceManager.closeAudioDevice();
        engine.getDeviceManager().removeHostedAudioDeviceInterface();
    }

    void timerCallback() override
    {
        const auto due = (juce::int64) ((juce::Time::getMillisecondCounterHiRes() - startTime) * 44.1);
        juce::MidiBuffer midi;

        while (fed + 512 <= due)
        {
            juce::AudioBuffer<float> buffer (2, 512);
            buffer.clear();

            for (int i = 0; i < 512; ++i)
            {
                const double t = (double) (fed + i) / 44100.0;
                const double wobble = 0.5 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 1.5 * t);
                buffer.setSample (0, i, (float) (0.4 * wobble * std::sin (juce::MathConstants<double>::twoPi * 220.0 * t)));
            }

            io.processBlock (buffer, midi);
            fed += 512;
        }
    }

    te::Engine& engine;
    te::HostedAudioDeviceInterface& io;
    double startTime = 0.0;
    juce::int64 fed = 0;
};


//==============================================================================
MainComponent::MainComponent (te::Engine& e)
    : engine (e),
      simulatedMic (juce::JUCEApplication::getCommandLineParameterArray().contains ("--simulate-mic")
                      ? std::make_unique<SimulatedMic> (e) : nullptr)
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

    for (auto* c : std::initializer_list<juce::Component*> {
             &hostButton, &joinButton, &leaveButton, &portEditor, &addressEditor, &portLabel, &addressLabel, &statusLabel,
             &patModeButton, &songModeButton, &playButton, &recordButton, &tempoLabel, &tempoSlider, &patternLabel, &patternBox, &newPatternButton, &patternMenuButton,
             &lengthBox, &playingLabel, &undoButton, &redoButton, &scanButton, &audioButton,
             &playlist, &divider1, &rack, &divider2, &bottomTabs, &logView })
        addAndMakeVisible (c);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setFont (juce::FontOptions (13.0f));
    statusLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    playingLabel.setColour (juce::Label::textColourId, accent);

    tempoSlider.setRange (40.0, 250.0, 1.0);
    tempoSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 26);
    tempoSlider.onDragStart = [this] { state.beginAction(); };
    tempoSlider.onValueChange = [this]
    {
        if (auto* edit = state.getEdit())
            if (auto* tempo = edit->tempoSequence.getTempo (0))
                if (std::abs (tempo->getBpm() - tempoSlider.getValue()) > 0.01)
                    tempo->setBpm (tempoSlider.getValue());
    };

    lengthBox.addItem ("1 bar", 1);
    lengthBox.addItem ("2 bars", 2);
    lengthBox.addItem ("4 bars", 4);
    lengthBox.setTooltip ("Length of the viewed pattern");
    lengthBox.onChange = [this]
    {
        auto pattern = state.getPattern();
        auto* edit = state.getEdit();

        if (edit != nullptr && pattern.isValid() && lengthBox.getSelectedId() != song::getBars (pattern))
        {
            state.beginAction();
            song::setBars (*edit, pattern, lengthBox.getSelectedId());
        }
    };

    patternBox.onChange = [this]
    {
        auto patterns = state.getEdit() != nullptr ? song::getPatterns (*state.getEdit()) : juce::Array<juce::ValueTree>();
        auto index = patternBox.getSelectedItemIndex();

        if (juce::isPositiveAndBelow (index, patterns.size()))
            selectPattern (patterns[index][song::ids::sid].toString());
    };

    newPatternButton.setTooltip ("New pattern");
    newPatternButton.onClick = [this]
    {
        if (auto* edit = state.getEdit())
        {
            state.beginAction();
            auto p = song::addPattern (*edit, song::nextPatternName (*edit), song::getBars (state.getPattern()));
            selectPattern (p[song::ids::sid].toString());
        }
    };

    patternMenuButton.setTooltip ("Rename / clone / delete pattern");
    patternMenuButton.onClick = [this] { showPatternMenu(); };

    state.onSelectionChanged = [this] { refresh(); };

    session.onLog = [this] (const juce::String& s) { appendLog (s); };
    session.onStatusChanged = [this] { triggerAsyncUpdate(); };
    session.onRemoteChange = [this] { triggerAsyncUpdate(); };
    session.onTransportChanged = [this] { triggerAsyncUpdate(); };
    session.onEditAboutToBeReplaced = [this] { unwatchEdit(); };
    session.onEditReplaced = [this] { watchEdit(); triggerAsyncUpdate(); };

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
        if (auto* edit = state.getEdit())
        {
            const bool play = ! edit->getTransport().isPlaying();

            if (play && ! session.isSongMode())
                session.setPlayingPattern (state.getPattern()[song::ids::sid].toString());

            // Play starts from the green marker (piano roll in PAT mode, playlist in SONG mode);
            // Stop returns to it, like FL.
            session.setPlaying (play, state.beatsToSeconds (state.getPlayStartBeat()));
        }

        refresh();
    };

    recordButton.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x8f Rec"));
    recordButton.setTooltip ("Record (R): records every armed mixer insert from the song start marker; "
                             "the others play along. Press Stop or Space to finish");
    recordButton.onClick = [this]
    {
        if (session.isRecording())
            session.setPlaying (false, state.beatsToSeconds (state.songStartBeat));
        else if (session.getRecorder().getNumArmed() == 0)
            showRecordSetupMenu();
        else if (session.startRecording (state.beatsToSeconds (state.songStartBeat)))
            appendLog ("Recording... press Stop or Space to finish");

        refresh();
    };

    const auto tabColour = juce::Colour (0xff2a2f36);
    bottomTabs.addTab ("Piano roll  (F7)", tabColour, &pianoRoll, false);
    bottomTabs.addTab ("Mixer  (F9)", tabColour, &mixer, false);
    bottomTabs.setTabBarDepth (26);
    bottomTabs.setOutline (0);

    // Panels: playlist, divider, rack, divider, piano roll / mixer tabs (sizes are proportions or pixels).
    panelLayout.setItemLayout (0, 110, -0.7, -0.30);
    panelLayout.setItemLayout (1, 6, 6, 6);
    panelLayout.setItemLayout (2, 110, -0.6, -0.31);
    panelLayout.setItemLayout (3, 6, 6, 6);
    panelLayout.setItemLayout (4, 120, -0.8, -0.39);

    for (auto* b : { &patModeButton, &songModeButton })
    {
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonOnColourId, accent.darker (0.2f));
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    }

    patModeButton.setTooltip ("Pattern mode: Play loops the pattern you are editing");
    songModeButton.setTooltip ("Song mode: Play plays the playlist");
    patModeButton.onClick = [this] { session.setSongMode (false); refresh(); };
    songModeButton.onClick = [this] { session.setSongMode (true); refresh(); };

    undoButton.onClick = [this] { undo (false); };
    redoButton.onClick = [this] { undo (true); };
    undoButton.setTooltip ("Undo your last change (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Y)");
    scanButton.onClick = [this] { showPluginScanner(); };
    audioButton.onClick = [this] { showAudioSettings(); };

    setWantsKeyboardFocus (true);

    // Two copies on one PC (testing alone) each get their own folder of audio
    // files, so the second copy really receives files like a second PC would.
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SyncTest");

        for (int i = 1; i <= 8; ++i)
        {
            auto lock = std::make_unique<juce::InterProcessLock> ("SyncTest-assets-" + juce::String (i));

            if (lock->enter (0))
            {
                assetFolderLock = std::move (lock);
                auto folder = base.getChildFile (i == 1 ? juce::String ("Assets") : "Assets-" + juce::String (i));
                session.setAssetFolder (folder);

                if (i > 1)
                    appendLog ("Copy " + juce::String (i) + " running on this PC: its audio files are kept apart in "
                                + folder.getFullPathName() + ", like a separate PC");
                break;
            }
        }
    }

    session.createLocalEdit();

    // Developer option for screenshots: SyncTest --demo-mixer
    if (juce::JUCEApplication::getCommandLineParameterArray().contains ("--demo-mixer"))
    {
        auto& edit = *session.getEdit();

        if (auto* ins = song::addInsert (edit, "Drums"))
        {
            for (auto* name : { "Kick", "Clap" })
                for (auto* t : song::getChannels (edit))
                    if (t->getName() == name)
                        song::setRoute (edit, *t, ins->itemID.toString());

            song::putEffect (edit, ins, song::createEffect (edit, te::CompressorPlugin::xmlTypeName, nullptr), -1);
            song::putEffect (edit, ins, song::createEffect (edit, te::EqualiserPlugin::xmlTypeName, nullptr), -1);
            state.selectedInsert = ins->itemID.toString();
        }

        song::addInsert (edit, "Vocals");
        bottomTabs.setCurrentTabIndex (1);
    }

    // Developer option for screenshots: SyncTest --demo-audio=<file>
    // (one real audio clip, plus one whose file "hasn't arrived yet").
    for (auto& a : juce::JUCEApplication::getCommandLineParameterArray())
    {
        if (! a.startsWith ("--demo-audio="))
            continue;

        auto& edit = *session.getEdit();
        const juce::File file (a.fromFirstOccurrenceOf ("=", false, false).unquoted());
        auto r = session.getAssetStore().importAudioFile (file);

        if (r.ok())
        {
            auto clip = song::addImportedAudio (edit, r, file.getFileName(), 1, 4.0);
            state.selectedChannel = clip[song::ids::ch].toString();
        }

        AssetStore::ImportResult missing;
        missing.hash = "00000000000000000000000000000000";
        missing.lengthSeconds = 6.0;
        song::addImportedAudio (edit, missing, "friend-vocal.wav", 2, 8.0);
        session.setSongMode (true);
    }

    appendLog ("SyncTest " + juce::JUCEApplication::getInstance()->getApplicationVersion()
                + ". First time: press 'Scan plugins...' so your VST3 plugins can be found.");
    setSize (1280, 900);
    startTimerHz (5);
}

MainComponent::~MainComponent()
{
    stopTimer();
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
    }
}

void MainComponent::unwatchEdit()
{
    watchedState.removeListener (this);
    watchedState = {};
}

//==============================================================================
void MainComponent::selectPattern (const juce::String& sid)
{
    state.viewedPattern = sid;
    state.placeAudioChannel = {};   // the playlist places this pattern again

    // FL pattern mode: while playing, switching pattern switches what plays (for everyone).
    if (auto* edit = state.getEdit())
        if (edit->getTransport().isPlaying() && ! session.isSongMode())
            session.setPlayingPattern (sid);

    refresh();
}

void MainComponent::refreshPatternControls()
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    auto patterns = song::getPatterns (*edit);
    auto viewed = state.getPattern();

    patternBox.clear (juce::dontSendNotification);

    for (int i = 0; i < patterns.size(); ++i)
        patternBox.addItem (patterns[i][song::ids::name].toString(), i + 1);

    patternBox.setSelectedItemIndex (patterns.indexOf (viewed), juce::dontSendNotification);
    lengthBox.setSelectedId (song::getBars (viewed), juce::dontSendNotification);

    const bool playing = edit->getTransport().isPlaying();
    auto playingPattern = song::findPattern (*edit, session.getPlayingPattern());
    const auto what = session.isSongMode() ? juce::String ("Song") : playingPattern[song::ids::name].toString();
    playingLabel.setText (playing ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + what : juce::String(),
                          juce::dontSendNotification);

    patModeButton.setToggleState (! session.isSongMode(), juce::dontSendNotification);
    songModeButton.setToggleState (session.isSongMode(), juce::dontSendNotification);
}

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
    {
        playButton.setButtonText (edit->getTransport().isPlaying() ? "Stop" : "Play");

        const bool recording = session.isRecording();
        recordButton.setColour (juce::TextButton::buttonColourId, recording ? juce::Colour (0xffd03a3a) : juce::Colour (0xff3a414a));
        recordButton.setColour (juce::TextButton::textColourOffId, recording ? juce::Colours::white : juce::Colour (0xffe06060));

        if (auto* tempo = edit->tempoSequence.getTempo (0))
            if (! tempoSlider.isMouseButtonDown())
                tempoSlider.setValue (tempo->getBpm(), juce::dontSendNotification);

        undoButton.setEnabled (edit->getUndoManager().canUndo());
        redoButton.setEnabled (edit->getUndoManager().canRedo());
    }

    refreshPatternControls();
    playlist.refresh();
    rack.refresh();
    pianoRoll.refresh();
    mixer.refresh();
}

void MainComponent::undo (bool redo)
{
    if (auto* edit = state.getEdit())
    {
        if (redo)
            edit->redo();
        else
            edit->undo();
    }

    refresh();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto cmd = juce::ModifierKeys::commandModifier;

    if (key == juce::KeyPress ('z', cmd, 0))                                        { undo (false); return true; }
    if (key == juce::KeyPress ('y', cmd, 0)
         || key == juce::KeyPress ('z', cmd | juce::ModifierKeys::shiftModifier, 0)) { undo (true); return true; }
    if (key == juce::KeyPress::spaceKey)                                            { playButton.triggerClick(); return true; }
    if (key == juce::KeyPress ('r'))                                                { recordButton.triggerClick(); return true; }
    if (key == juce::KeyPress::F7Key)                                               { bottomTabs.setCurrentTabIndex (0); return true; }
    if (key == juce::KeyPress::F9Key)                                               { bottomTabs.setCurrentTabIndex (1); return true; }

    return false;
}

void MainComponent::showPatternMenu()
{
    auto* edit = state.getEdit();
    auto pattern = state.getPattern();

    if (edit == nullptr || ! pattern.isValid())
        return;

    juce::PopupMenu m;
    m.addItem (1, "Rename...");
    m.addItem (2, "Clone");
    m.addItem (3, "Delete", song::getPatterns (*edit).size() > 1);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (patternMenuButton), [this, pattern] (int r)
    {
        auto* e = state.getEdit();

        if (e == nullptr || r == 0 || ! pattern.getParent().isValid())
            return;

        if (r == 1)
        {
            auto* w = new juce::AlertWindow ("Rename pattern", {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", pattern[song::ids::name].toString());
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, pattern, w] (int res) mutable
            {
                if (res == 1 && state.getEdit() != nullptr && pattern.getParent().isValid())
                {
                    state.beginAction();
                    pattern.setProperty (song::ids::name, w->getTextEditorContents ("name").trim().substring (0, 40),
                                         &state.getEdit()->getUndoManager());
                }
            }), true);
        }
        else if (r == 2)
        {
            state.beginAction();
            auto copy = song::addPattern (*e, pattern[song::ids::name].toString() + " copy", song::getBars (pattern));

            for (auto n : pattern)
                if (n.hasType (song::ids::PNOTE))
                    song::addNote (*e, copy, n[song::ids::ch].toString(), n[song::ids::p], n[song::ids::b],
                                   n[song::ids::l], n[song::ids::v]);

            selectPattern (copy[song::ids::sid].toString());
        }
        else if (r == 3)
        {
            state.beginAction();
            song::removePattern (*e, pattern);
            state.viewedPattern = {};
            refresh();
        }
    });
}

void MainComponent::timerCallback()
{
    // Recording starts/ends a moment after the button (the engine applies it).
    const bool recording = session.isRecording();

    if (recording != lastRecording)
    {
        lastRecording = recording;
        refresh();
    }
}

void MainComponent::showRecordSetupMenu()
{
    // Nothing armed yet: offer to make a "Vocals" insert recording from an input.
    auto inputs = session.getRecorder().getInputs();

    juce::PopupMenu m;
    m.addSectionHeader ("Nothing is armed. Record from:");

    for (int i = 0; i < inputs.size(); ++i)
        m.addItem (100 + i, inputs[i].name);

    if (inputs.isEmpty())
        m.addItem (-1, "(no inputs found)", false);

    m.addSeparator();
    m.addItem (1, "Audio settings...");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordButton), [this, inputs] (int r)
    {
        auto* edit = state.getEdit();

        if (r == 1)
            showAudioSettings();

        if (edit == nullptr || ! juce::isPositiveAndBelow (r - 100, inputs.size()))
            return;

        // A free name: Vocals, Vocals 2...
        juce::String name ("Vocals");

        for (int n = 2;; ++n)
        {
            bool used = false;

            for (auto* t : song::getInserts (*edit))
                if (t->getName() == name)
                    used = true;

            if (! used)
                break;

            name = "Vocals " + juce::String (n);
        }

        state.beginAction();

        if (auto* insert = song::addInsert (*edit, name))
        {
            const auto id = insert->itemID.toString();
            session.getRecorder().setInputFor (id, inputs[r - 100].id);
            state.selectedInsert = id;
            bottomTabs.setCurrentTabIndex (1);
            appendLog ("Insert \"" + name + "\" records from " + inputs[r - 100].name
                        + " (you hear it through its effects). Add autotune/EQ to it if you like, then press Rec.");
        }

        refresh();
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
    auto r = getLocalBounds().reduced (10);

    auto top = r.removeFromTop (28);
    hostButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (8);
    portLabel.setBounds (top.removeFromLeft (36));
    portEditor.setBounds (top.removeFromLeft (64));
    top.removeFromLeft (12);
    addressLabel.setBounds (top.removeFromLeft (92));
    addressEditor.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (8);
    joinButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (8);
    leaveButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (16);
    statusLabel.setBounds (top);

    r.removeFromTop (8);
    auto bar = r.removeFromTop (28);
    // Right-hand buttons first so they always fit; the "now playing" label gets what is left.
    audioButton.setBounds (bar.removeFromRight (124));
    bar.removeFromRight (6);
    scanButton.setBounds (bar.removeFromRight (116));
    bar.removeFromRight (12);
    redoButton.setBounds (bar.removeFromRight (56));
    bar.removeFromRight (4);
    undoButton.setBounds (bar.removeFromRight (56));
    bar.removeFromRight (8);

    patModeButton.setBounds (bar.removeFromLeft (48));
    songModeButton.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (6);
    playButton.setBounds (bar.removeFromLeft (70));
    bar.removeFromLeft (4);
    recordButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (12);
    tempoLabel.setBounds (bar.removeFromLeft (36));
    tempoSlider.setBounds (bar.removeFromLeft (110));
    bar.removeFromLeft (12);
    patternLabel.setBounds (bar.removeFromLeft (56));
    patternBox.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (4);
    newPatternButton.setBounds (bar.removeFromLeft (28));
    bar.removeFromLeft (4);
    patternMenuButton.setBounds (bar.removeFromLeft (32));
    bar.removeFromLeft (8);
    lengthBox.setBounds (bar.removeFromLeft (80));
    bar.removeFromLeft (8);
    playingLabel.setBounds (bar);

    r.removeFromTop (8);
    logView.setBounds (r.removeFromBottom (70));
    r.removeFromBottom (8);

    juce::Component* panels[] = { &playlist, &divider1, &rack, &divider2, &bottomTabs };
    panelLayout.layOutComponents (panels, 5, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
}

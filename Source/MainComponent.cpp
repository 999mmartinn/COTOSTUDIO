#include "MainComponent.h"
#include "PatternCompiler.h"
#include "ProjectFile.h"
#include "AppData.h"

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
/** The mixer in a window of its own (e.g. full screen on a second monitor).
    Closing it puts the mixer back in the main window. */
class MainComponent::MixerWindow  : public juce::DocumentWindow
{
public:
    explicit MixerWindow (MainComponent& o)
        : DocumentWindow ("Mixer  -  COTOSTUDIO", juce::Colour (0xff14111c), DocumentWindow::allButtons), owner (o)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (480, 260, 8000, 5000);
        setContentNonOwned (&owner.mixer, false);

        const auto saved = owner.settings()->getValue ("mixerWindow");

        if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
        {
            // First time: on the second screen if there is one, otherwise over the main window.
            auto& displays = juce::Desktop::getInstance().getDisplays();
            auto area = displays.getPrimaryDisplay() != nullptr ? displays.getPrimaryDisplay()->userArea : juce::Rectangle<int> (0, 0, 1280, 800);

            for (auto& d : displays.displays)
                if (! d.isMain)
                    area = d.userArea;

            setBounds (area.withSizeKeepingCentre (juce::jmin (1400, area.getWidth() - 80), juce::jmin (700, area.getHeight() - 80)));
        }

        setVisible (true);
        ready = true;
    }

    ~MixerWindow() override
    {
        clearContentComponent();   // the mixer belongs to the main window
    }

    void closeButtonPressed() override                        { owner.setMixerDetached (false); }
    bool keyPressed (const juce::KeyPress& key) override      { return owner.keyPressed (key); }   // Space, Ctrl+S... work here too
    void moved() override                                     { DocumentWindow::moved(); remember(); }
    void resized() override                                   { DocumentWindow::resized(); remember(); }

private:
    MainComponent& owner;
    bool ready = false;

    void remember()
    {
        if (ready)
            owner.settings()->setValue ("mixerWindow", getWindowStateAsString());
    }
};

/** What the Mixer tab shows while the mixer has its own window. */
class MainComponent::MixerPlaceholder  : public juce::Component
{
public:
    explicit MixerPlaceholder (MainComponent& o) : owner (o)
    {
        backButton.onClick = [this] { owner.setMixerDetached (false); };
        showButton.onClick = [this] { owner.showMixer(); };
        addAndMakeVisible (backButton);
        addAndMakeVisible (showButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1724));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText ("The mixer is in its own window.", getLocalBounds().withTrimmedBottom (getHeight() / 2 - 10),
                          juce::Justification::centredBottom, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedTop (getHeight() / 2 + 6).removeFromTop (30).withSizeKeepingCentre (380, 30);
        showButton.setBounds (r.removeFromLeft (170));
        r.removeFromLeft (10);
        backButton.setBounds (r);
    }

private:
    MainComponent& owner;
    juce::TextButton backButton { "Bring it back here" }, showButton { "Show the window" };
};

//==============================================================================
MainComponent::MainComponent (te::Engine& e)
    : engine (e),
      simulatedMic (juce::JUCEApplication::getCommandLineParameterArray().contains ("--simulate-mic")
                      ? std::make_unique<SimulatedMic> (e) : nullptr)
{
    // Settings live in the data folder (D:\COTOSTUDIO\Settings), like everything else.
    juce::PropertiesFile::Options opts;
    opts.applicationName = appdata::appName;
    opts.filenameSuffix = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    opts.millisecondsBeforeSaving = 2000;
    userSettings = std::make_unique<juce::PropertiesFile> (appdata::settingsFolder().getChildFile ("COTOSTUDIO.settings"), opts);

    auto* prefs = userSettings.get();
    session.setUserName (prefs->getValue ("userName", juce::SystemStats::getComputerName()));
    browserVisible = prefs->getBoolValue ("browserVisible", true);
    soundsVisible = prefs->getBoolValue ("soundsVisible", true);
    recentFiles.setMaxNumberOfItems (8);
    recentFiles.restoreFromString (prefs->getValue ("recentSongs"));
    portEditor.setText (prefs->getValue ("port", "47800"));
    session.setMetronome (prefs->getBoolValue ("metronome", false));
    session.setCountInBars (prefs->getIntValue ("countInBars", 0));
    addressEditor.setText (prefs->getValue ("address", "127.0.0.1"));
    portEditor.setInputRestrictions (5, "0123456789");

    for (auto* c : std::initializer_list<juce::Component*> {
             &fileButton, &soundsButton, &hostButton, &joinButton, &leaveButton, &portEditor, &addressEditor, &portLabel, &addressLabel, &statusLabel,
             &patModeButton, &songModeButton, &shareButton, &focusButton, &playButton, &recordButton, &clickButton, &tempoLabel, &tempoSlider, &patternBox, &newPatternButton, &patternMenuButton,
             &lengthBox, &playingLabel, &undoButton, &redoButton, &scanButton, &audioButton,
             &sounds, &browser, &showBrowserButton, &playlist, &divider1, &rack, &divider2, &bottomTabs, &logView })
        addAndMakeVisible (c);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setFont (juce::FontOptions (13.0f));
    statusLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    playingLabel.setColour (juce::Label::textColourId, accent);

    tempoSlider.setRange (40.0, 250.0, 1.0);
    tempoSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 26);
    tempoSlider.setTooltip ("Tempo. In a session everyone else has to accept a change (it is part of the song)");
    tempoSlider.onValueChange = [this]
    {
        if (! session.tempoNeedsApproval())
        {
            session.requestTempo (tempoSlider.getValue());   // alone: right away
            return;
        }

        // In a session: wait until you stop clicking, then ask everyone once.
        tempoAskCountdown = 4;
        refreshTempo();
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

    // Browser (left): patterns, audio files, people + chat.
    browser.onPatternChosen = [this] (const juce::String& sid) { selectPattern (sid); };
    browser.onCollapse = [this] { setBrowserVisible (false); };
    browser.onImportAudio = [this] { playlist.showImportDialog(); };
    browser.onUserNameChanged = [this] (const juce::String& name) { settings()->setValue ("userName", name); };
    showBrowserButton.setTooltip ("Show the browser (F8)");
    showBrowserButton.onClick = [this] { setBrowserVisible (true); };
    // Your sounds (far left): folders saved on this PC only; a sound joins the song when dropped in.
    sounds.setFolders (juce::StringArray::fromLines (prefs->getValue ("soundFolders")));
    sounds.setPreviewVolume ((float) prefs->getDoubleValue ("previewVolume", 0.8));
    sounds.restoreState (lastSoundsState = prefs->getValue ("soundsState"));   // where you left it
    sounds.onFoldersChanged = [this] (const juce::StringArray& folders)
    {
        settings()->setValue ("soundFolders", folders.joinIntoString ("\n"));
    };
    sounds.onCollapse = [this] { setSoundsVisible (false); };
    sounds.onAddToRack = [this] (const juce::File& f) { rack.addSoundFile (f); };
    sounds.onAddToPlaylist = [this] (const juce::File& f)
    {
        if (auto* edit = state.getEdit())
            playlist.importFiles ({ f }, song::findFreeLane (*edit, state.songStartBeat, state.songStartBeat + 4.0), state.songStartBeat);
    };
    sounds.onMidiToSelectedChannel = [this] (const juce::File& f)
    {
        auto* channel = state.getSelectedChannel();
        rack.importMidiFile (f, channel != nullptr ? song::getChannelID (*channel) : juce::String());
    };
    sounds.onMidiToNewChannel = [this] (const juce::File& f) { rack.importMidiFile (f); };

    soundsButton.setClickingTogglesState (false);
    soundsButton.setColour (juce::TextButton::buttonOnColourId, accent.darker (0.2f));
    soundsButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    soundsButton.setTooltip ("Your folders of sounds and MIDI (Alt+F8). Only on this PC until you drag a sound into the song");
    soundsButton.onClick = [this] { setSoundsVisible (! soundsVisible); };

    session.onPeopleChanged = [this] { browser.refreshPeople(); triggerAsyncUpdate(); };
    session.onSharedPatternOffered = [this] (const juce::String& sid, const juce::String& owner) { showPatternOffer (sid, owner); };
    session.onTempoQuestion = [this] (const collab::Session::TempoQuestion& q) { showTempoQuestion (q); };
    session.onTempoQuestionClosed = [this] (int id) { closeTempoQuestion (id); };
    session.onChatChanged = [this] { browser.refreshChat(); };
    browser.refreshPeople();

    session.onLog = [this] (const juce::String& s) { appendLog (s); };
    session.onStatusChanged = [this] { triggerAsyncUpdate(); };
    session.onRemoteChange = [this] { triggerAsyncUpdate(); };
    session.onTransportChanged = [this] { triggerAsyncUpdate(); };
    session.onEditAboutToBeReplaced = [this] { unwatchEdit(); };
    session.onEditReplaced = [this]
    {
        watchEdit();

        // A song from somewhere else (joining, or the host switched songs) isn't the
        // file this PC was saving to; a fresh copy of the same song (resync) still is.
        const auto reason = session.getLastReplaceReason();

        if (! expectingOwnSong && (reason == "join" || reason == "newSong" || reason == "own"))
        {
            currentFile = juce::File();
            savedHash = lastAutosaveHash = currentHash();
            dirty = false;
        }

        triggerAsyncUpdate();
    };

    fileButton.setTooltip ("New, open and save songs (Ctrl+N / Ctrl+O / Ctrl+S)");
    fileButton.onClick = [this] { showFileMenu(); };

    hostButton.onClick = [this]
    {
        settings()->setValue ("port", portEditor.getText());
        session.startHosting (portEditor.getText().getIntValue());
    };

    joinButton.onClick = [this]
    {
        settings()->setValue ("port", portEditor.getText());
        settings()->setValue ("address", addressEditor.getText());
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
    recordButton.setTooltip ("Record (R): records every armed mixer insert from the song start marker. "
                             "Press Stop or Space to finish. Right-click: count-in");

    clickButton.setClickingTogglesState (false);
    clickButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff4fb0f0));
    clickButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    clickButton.setTooltip ("Metronome on/off (only you hear it). Right-click: count-in before recording");
    clickButton.onClick = [this]
    {
        if (clickButton.wasRightClick())
        {
            showRecordOptionsMenu (&clickButton);
            return;
        }

        session.setMetronome (! session.isMetronomeOn());
        settings()->setValue ("metronome", session.isMetronomeOn());
        refresh();
    };
    recordButton.onClick = [this]
    {
        if (recordButton.wasRightClick())
        {
            showRecordOptionsMenu (&recordButton);
            return;
        }

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
    bottomTabs.addTab ("Automation", tabColour, &automationEditor, false);

    state.createAutomation = [this] (const juce::String& pluginID, const juce::String& paramID) { createAutomation (pluginID, paramID); };
    state.onAutomationSelected = [this]
    {
        bottomTabs.setCurrentTabIndex (2);
        refresh();
    };
    bottomTabs.setTabBarDepth (26);
    bottomTabs.setOutline (0);

    // The mixer can have a window of its own (e.g. full screen on a second monitor).
    mixerPlaceholder = std::make_unique<MixerPlaceholder> (*this);
    mixer.onDetachToggle = [this] { setMixerDetached (mixerWindow == nullptr); };
    mixer.onFullScreenToggle = [this]
    {
        if (mixerWindow != nullptr)
            mixerWindow->setFullScreen (! mixerWindow->isFullScreen());
    };

    if (prefs->getBoolValue ("mixerDetached", false))
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr)
                safe->setMixerDetached (true);
        });

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

    // Share: everyone (except people in Focus) hears your playback. Focus: nobody's share pulls you in.
    for (auto* b : { &shareButton, &focusButton })
        b->setClickingTogglesState (false);

    shareButton.onClick = [this]
    {
        if (session.isSharing())
            session.stopSharing();
        else if (session.isFollowingShare())
            session.leaveShare();
        else if (session.isShareRunning())
            session.joinShare();
        else
        {
            const auto error = session.startSharing();

            if (error.isNotEmpty())
                appendLog (error);
        }

        refresh();
    };

    focusButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff8a6fd8));
    focusButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    focusButton.setTooltip ("Focus: when someone shares their playback, you keep working on your own and aren't pulled in");
    focusButton.onClick = [this] { session.setFocus (! session.isFocus()); refresh(); };

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
        for (int i = 1; i <= 8; ++i)
        {
            auto lock = std::make_unique<juce::InterProcessLock> ("COTOSTUDIO-assets-" + juce::String (i));

            if (lock->enter (0))
            {
                assetFolderLock = std::move (lock);
                auto folder = appdata::audioCacheFolder (i);
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
    saveSoundsState();
    mixerWindow.reset();
    settings()->setValue ("previewVolume", (double) previewer.getGain());
    session.clearCallbacks();   // the session goes after the window's parts: it must not call into them
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
    const auto whose = session.isFollowingShare() ? session.getShareOwnerName() + ": " : juce::String();
    playingLabel.setText (playing ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + whose + what
                                     + (session.isSharing() ? "  (shared)" : "")
                                  : juce::String(),
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

        refreshTempo();

        undoButton.setEnabled (edit->getUndoManager().canUndo());
        redoButton.setEnabled (edit->getUndoManager().canRedo());
    }

    // Sharing / Focus
    {
        const bool inSession = mode == Mode::hosting || mode == Mode::joined;
        const auto owner = session.getShareOwnerName().substring (0, 14);
        juce::Colour on = accent.darker (0.2f);

        if (session.isSharing())
        {
            shareButton.setButtonText ("Sharing");
            shareButton.setTooltip ("Everyone following hears your playback, in sync. Only you control it. Click to stop sharing");
        }
        else if (session.isFollowingShare())
        {
            on = session.getShareOwnerColour();
            shareButton.setButtonText ("With " + owner);
            shareButton.setTooltip (session.getShareOwnerName() + " controls the playback. Click to leave "
                                    "(you go back to where you were; the others keep listening)");
        }
        else if (session.isShareRunning())
        {
            shareButton.setButtonText ("Join " + owner);
            shareButton.setTooltip (session.getShareOwnerName() + " is sharing their playback. Click to listen along");
        }
        else
        {
            shareButton.setButtonText ("Share");
            shareButton.setTooltip (inSession ? "Share your playback: everyone (except people in Focus) hears what you play, "
                                                "in sync, and only you control it. Click again to stop"
                                              : "Share your playback with the others (in a session)");
        }

        shareButton.setColour (juce::TextButton::buttonOnColourId, on);
        shareButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        shareButton.setToggleState (session.isSharing() || session.isFollowingShare(), juce::dontSendNotification);
        shareButton.setEnabled (inSession);
        focusButton.setToggleState (session.isFocus(), juce::dontSendNotification);
        clickButton.setToggleState (session.isMetronomeOn(), juce::dontSendNotification);
        clickButton.setButtonText (session.getCountInBars() > 0 ? "Click +" + juce::String (session.getCountInBars())
                                                                : juce::String ("Click"));

        // Following someone's share: only they control playback.
        const bool control = session.canControlPlayback();

        for (auto* b : std::initializer_list<juce::Button*> { &playButton, &recordButton, &patModeButton, &songModeButton })
            b->setEnabled (control);

        if (! session.isFollowingShare() && patternOffer != nullptr)
            patternOffer->exitModalState (0);   // the share is over: the question is moot
    }

    refreshPatternControls();
    session.setViewingPattern (state.getPattern()[song::ids::sid].toString());   // the others see where you are
    updateTitle();
    browser.refresh();
    playlist.refresh();
    rack.refresh();
    pianoRoll.refresh();
    mixer.refresh();
    automationEditor.refresh();
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
    if (key == juce::KeyPress ('s', cmd, 0))                                        { saveSong (false); return true; }
    if (key == juce::KeyPress ('s', cmd | juce::ModifierKeys::shiftModifier, 0))     { saveSong (true); return true; }
    if (key == juce::KeyPress ('o', cmd, 0))                                        { openSong(); return true; }
    if (key == juce::KeyPress ('n', cmd, 0))                                        { newSong(); return true; }
    if (key == juce::KeyPress ('r', cmd, 0))                                        { showExportDialog(); return true; }
    if (key == juce::KeyPress ('y', cmd, 0)
         || key == juce::KeyPress ('z', cmd | juce::ModifierKeys::shiftModifier, 0)) { undo (true); return true; }
    if ((key == juce::KeyPress::spaceKey || key == juce::KeyPress ('r')) && ! session.canControlPlayback())
    {
        appendLog (session.getShareOwnerName() + " controls the playback right now (click 'With "
                    + session.getShareOwnerName() + "' to leave and play on your own)");
        return true;
    }

    if (key == juce::KeyPress::spaceKey)                                            { playButton.triggerClick(); return true; }
    if (key == juce::KeyPress ('r'))                                                { recordButton.triggerClick(); return true; }
    if (key == juce::KeyPress::F7Key)                                               { bottomTabs.setCurrentTabIndex (0); return true; }
    if (key == juce::KeyPress::F9Key)                                               { showMixer(); return true; }
    if (key == juce::KeyPress (juce::KeyPress::F8Key, juce::ModifierKeys::altModifier, 0)) { setSoundsVisible (! soundsVisible); return true; }
    if (key == juce::KeyPress::F8Key)                                               { setBrowserVisible (! browserVisible); return true; }

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
    m.addItem (4, "Split by channel", song::getPatternChannels (*edit, pattern).size() >= 2);
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
            auto copy = song::clonePattern (*e, pattern);
            selectPattern (copy[song::ids::sid].toString());
        }
        else if (r == 4)
        {
            state.beginAction();
            auto parts = song::splitPatternByChannel (*e, pattern);

            if (! parts.isEmpty())
                selectPattern (parts[0][song::ids::sid].toString());
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

void MainComponent::saveSoundsState()
{
    const auto now = sounds.getState();

    if (now != lastSoundsState)
    {
        lastSoundsState = now;
        settings()->setValue ("soundsState", now);
    }
}

void MainComponent::showMixer()
{
    if (mixerWindow != nullptr)
        mixerWindow->toFront (true);
    else
        bottomTabs.setCurrentTabIndex (1);
}

void MainComponent::setMixerDetached (bool detached)
{
    if (detached == (mixerWindow != nullptr))
        return;

    const auto colour = bottomTabs.getTabBackgroundColour (1);

    if (detached)
    {
        // The Mixer tab keeps its place (with a note); the mixer moves to its window.
        bottomTabs.removeTab (1);
        bottomTabs.addTab ("Mixer  (F9)", colour, mixerPlaceholder.get(), false, 1);
        mixerWindow = std::make_unique<MixerWindow> (*this);
        appendLog ("The mixer has its own window now: move it to your other screen (F9 brings it to the front)");
    }
    else
    {
        mixerWindow.reset();
        bottomTabs.removeTab (1);
        bottomTabs.addTab ("Mixer  (F9)", colour, &mixer, false, 1);
        bottomTabs.setCurrentTabIndex (1);
    }

    mixer.setDetached (detached);
    settings()->setValue ("mixerDetached", detached);
}

void MainComponent::setSoundsVisible (bool shouldBeVisible)
{
    soundsVisible = shouldBeVisible;
    settings()->setValue ("soundsVisible", soundsVisible);
    resized();
}

void MainComponent::setBrowserVisible (bool shouldBeVisible)
{
    browserVisible = shouldBeVisible;
    settings()->setValue ("browserVisible", browserVisible);
    resized();
}

void MainComponent::timerCallback()
{
    // Unsaved changes: compare the song with how it was when last saved (every 2 s, if it changed).
    if (++ticksSinceHashCheck >= 10 && songChanged)
    {
        ticksSinceHashCheck = 0;
        songChanged = false;
        const bool nowDirty = currentHash() != savedHash;

        if (nowDirty != dirty)
        {
            dirty = nowDirty;
            updateTitle();
        }
    }

    // Where you are in your sounds (open folders, scroll, the sound you picked), every 2 s if it changed.
    if (++ticksSinceSoundsSave >= 10)
    {
        ticksSinceSoundsSave = 0;
        saveSoundsState();
    }

    // A backup every 3 minutes while there are changes.
    if (++ticksSinceAutosave >= 5 * 180)
    {
        ticksSinceAutosave = 0;
        autosave();
    }

    if (exporter.isRunning())
        exportProgress = exporter.getProgress();

    // A changed tempo is asked for once you stop clicking.
    if (tempoAskCountdown > 0 && --tempoAskCountdown == 0)
    {
        session.requestTempo (tempoSlider.getValue());
        refreshTempo();
    }

    // Recording starts/ends a moment after the button (the engine applies it).
    const bool recording = session.isRecording();

    if (recording != lastRecording)
    {
        lastRecording = recording;
        refresh();
    }
}

//==============================================================================
void MainComponent::showPatternOffer (const juce::String& sid, const juce::String& owner)
{
    auto* edit = state.getEdit();
    auto pattern = edit != nullptr ? song::findPattern (*edit, sid) : juce::ValueTree();

    if (! pattern.isValid())
        return;

    if (patternOffer != nullptr)
        patternOffer->exitModalState (0);   // an older offer (they switched pattern again)

    auto* w = new juce::AlertWindow ("Go to the shared pattern?",
                                     owner + " is playing \"" + pattern[song::ids::name].toString() + "\".\n"
                                     "Go to that pattern? (If you stay, you keep working on yours and still hear theirs.)",
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addButton ("Go there", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Stay here", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    patternOffer = w;

    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, sid] (int result)
    {
        if (result == 1 && session.isFollowingShare() && state.getEdit() != nullptr
             && song::findPattern (*state.getEdit(), sid).isValid())
            selectPattern (sid);
    }), true);
}

void MainComponent::showTempoQuestion (const collab::Session::TempoQuestion& q)
{
    auto bpm = [] (double v) { return std::abs (v - std::round (v)) < 0.005 ? juce::String (juce::roundToInt (v)) : juce::String (v, 2); };

    auto* w = new juce::AlertWindow ("Change the tempo?",
                                     q.fromName + " wants to change the tempo from " + bpm (q.oldBpm) + " to " + bpm (q.bpm)
                                       + " BPM.\nEveryone has to agree (30 seconds to answer).",
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addButton ("Accept", 1);
    w->addButton ("Decline", 0);
    tempoQuestions[q.id] = w;

    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, id = q.id] (int result)
    {
        auto it = tempoQuestions.find (id);

        if (it == tempoQuestions.end())
            return;   // closed because it was already decided

        tempoQuestions.erase (it);
        session.answerTempo (id, result == 1);
    }), true);
}

void MainComponent::closeTempoQuestion (int id)
{
    auto it = tempoQuestions.find (id);

    if (it == tempoQuestions.end())
        return;

    auto w = it->second;
    tempoQuestions.erase (it);

    if (w != nullptr)
        w->exitModalState (0);
}

void MainComponent::refreshTempo()
{
    auto* edit = state.getEdit();
    auto* tempo = edit != nullptr ? edit->tempoSequence.getTempo (0) : nullptr;

    if (tempo == nullptr)
        return;

    // (The new tempo can arrive a moment before the "approved" notice: then it's done already.)
    const double pending = std::abs (session.getPendingTempo() - tempo->getBpm()) < 0.001 ? 0.0 : session.getPendingTempo();
    const bool waiting = tempoAskCountdown > 0 || pending != 0.0;

    if (tempoAskCountdown == 0 && ! tempoSlider.isMouseButtonDown())
        tempoSlider.setValue (pending != 0.0 ? pending : tempo->getBpm(), juce::dontSendNotification);

    // Orange: a tempo you asked for that isn't in the song yet.
    tempoSlider.setColour (juce::Slider::textBoxTextColourId, waiting ? juce::Colour (0xffffa040) : juce::Colours::white);
    tempoSlider.setTooltip (pending != 0.0 ? "Waiting for everyone to accept " + juce::String (pending, 0) + " BPM "
                                               "(the song is still at " + juce::String (tempo->getBpm(), 0) + ")"
                                           : "Tempo. In a session everyone else has to accept a change (it is part of the song)");
}

void MainComponent::createAutomation (const juce::String& pluginID, const juce::String& paramID)
{
    auto* edit = state.getEdit();
    auto* param = edit != nullptr ? song::findParameter (*edit, pluginID, paramID) : nullptr;

    if (param == nullptr)
        return;

    // Like FL: a new clip on a free track, across the whole song, starting at the knob's current value.
    const double length = juce::jmax (16.0, song::getSongLengthBeats (*edit));
    const float value = param->getCurrentNormalisedValue();

    state.beginAction();
    auto clip = song::addAutomationClip (*edit, pluginID, paramID, song::findFreeLane (*edit, 0.0, length), 0.0, length, value);

    if (! clip.isValid())
        return;

    state.selectedAutomation = clip[song::ids::sid].toString();
    bottomTabs.setCurrentTabIndex (2);
    appendLog ("Automation clip for " + song::describeParameter (*edit, pluginID, paramID)
                + ": shape it in the Automation tab. It plays in SONG mode, for everyone");
    refresh();
}

void MainComponent::showRecordOptionsMenu (juce::Component* target)
{
    juce::PopupMenu m;
    const int bars = session.getCountInBars();
    m.addSectionHeader ("Count-in before recording (metronome)");
    m.addItem (1, "None", true, bars == 0);
    m.addItem (2, "1 bar", true, bars == 1);
    m.addItem (3, "2 bars", true, bars == 2);
    m.addSeparator();
    m.addItem (4, "Metronome while playing", true, session.isMetronomeOn());

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [this] (int r)
    {
        if (r >= 1 && r <= 3)
        {
            session.setCountInBars (r - 1);
            settings()->setValue ("countInBars", r - 1);
            appendLog (r == 1 ? juce::String ("Count-in off: recording starts right away")
                              : "Count-in: " + juce::String (r - 1) + " bar(s) of metronome before the take (it still starts at the marker)");
        }
        else if (r == 4)
        {
            session.setMetronome (! session.isMetronomeOn());
            settings()->setValue ("metronome", session.isMetronomeOn());
        }

        refresh();
    });
}

//==============================================================================
juce::File MainComponent::exportsFolder() const
{
    // A saved song exports into its own project folder.
    if (currentFile != juce::File() && ! appdata::isBackup (currentFile))
        return appdata::projectFolderOf (currentFile).getChildFile ("Exports");

    return appdata::exportsFolder();
}

void MainComponent::showExportDialog()
{
    auto* edit = state.getEdit();

    if (edit == nullptr || exporter.isRunning())
        return;

    auto* w = new juce::AlertWindow ("Export audio",
                                     "Renders on this PC with your plugins and audio files. "
                                     "Changes made meanwhile (by anyone) don't get into it.",
                                     juce::MessageBoxIconType::NoIcon);

    const auto patternName = state.getPattern()[song::ids::name].toString();
    w->addComboBox ("what", { "The whole song (playlist)", "Pattern \"" + patternName + "\" (as a seamless loop)" }, "What");

    juce::StringArray formats;
    juce::Array<Exporter::Format> formatValues;

    for (auto f : { Exporter::Format::wav24, Exporter::Format::wav16, Exporter::Format::flac,
                    Exporter::Format::mp3_320, Exporter::Format::mp3_192 })
    {
        const bool isMp3 = f == Exporter::Format::mp3_320 || f == Exporter::Format::mp3_192;

        if (isMp3 && ! Exporter::canExportMp3())
            continue;

        formats.add (Exporter::describe (f));
        formatValues.add (f);
    }

    w->addComboBox ("format", formats, "Format");
    w->addComboBox ("stems", { "Just the mix", "The mix + one file per mixer insert (stems)" }, "Files");

    auto* prefs = settings();
    w->getComboBoxComponent ("what")->setSelectedItemIndex (session.isSongMode() ? 0 : 1, juce::dontSendNotification);
    w->getComboBoxComponent ("format")->setSelectedItemIndex (juce::jlimit (0, formats.size() - 1, prefs->getIntValue ("exportFormat", 0)),
                                                              juce::dontSendNotification);
    w->getComboBoxComponent ("stems")->setSelectedItemIndex (prefs->getBoolValue ("exportStems", false) ? 1 : 0, juce::dontSendNotification);

    w->addButton ("Export...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    const auto patternSid = state.getPattern()[song::ids::sid].toString();

    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, formatValues, patternSid, patternName] (int result)
    {
        if (result != 1)
            return;

        Exporter::Options opts;
        opts.wholeSong = w->getComboBoxComponent ("what")->getSelectedItemIndex() == 0;
        opts.patternSid = patternSid;
        opts.format = formatValues[juce::jmax (0, w->getComboBoxComponent ("format")->getSelectedItemIndex())];
        opts.stems = w->getComboBoxComponent ("stems")->getSelectedItemIndex() == 1;

        auto* s = settings();
        s->setValue ("exportFormat", w->getComboBoxComponent ("format")->getSelectedItemIndex());
        s->setValue ("exportStems", opts.stems);

        // Where: a file name (the stems go in a folder next to it).
        exportsFolder().createDirectory();
        const auto title = session.getSongTitle().isNotEmpty() ? session.getSongTitle() : juce::String ("Untitled");
        const auto base = juce::File::createLegalFileName (opts.wholeSong ? title : title + " - " + patternName);
        const auto ext = Exporter::extensionFor (opts.format);

        fileChooser = std::make_unique<juce::FileChooser> ("Export to", exportsFolder().getChildFile (base + ext), "*" + ext);
        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this, opts] (const juce::FileChooser& fc) mutable
        {
            if (fc.getResult() == juce::File())
                return;

            opts.destination = fc.getResult();
            startExport (opts);
        });
    }), true);
}

void MainComponent::startExport (const Exporter::Options& opts)
{
    auto* sync = session.getSync();

    if (sync == nullptr)
        return;

    const auto error = exporter.start (sync->createSnapshot(), opts, [this] (const Exporter::Result& r) { exportFinished (r); });

    if (error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export", error);
        return;
    }

    appendLog ("Exporting to " + opts.destination.getFileName() + "...");
    exportProgress = 0.0;

    auto* w = new juce::AlertWindow ("Exporting...", opts.destination.getFileName(), juce::MessageBoxIconType::NoIcon);
    w->addProgressBarComponent (exportProgress);
    w->addButton ("Cancel", 0);
    exportWindow = w;

    w->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int)
    {
        if (exporter.isRunning())
            exporter.cancel();   // Cancel pressed (the window also closes itself when done)
    }), true);
}

void MainComponent::exportFinished (const Exporter::Result& r)
{
    if (exportWindow != nullptr)
    {
        auto w = exportWindow;
        exportWindow = nullptr;
        w->exitModalState (1);
    }

    if (r.cancelled)
    {
        appendLog ("Export cancelled");
        return;
    }

    if (r.error.isNotEmpty())
    {
        appendLog ("Export failed: " + r.error);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export failed", r.error);
        return;
    }

    juce::String text;
    juce::int64 bytes = 0;

    for (auto& f : r.files)
        bytes += f.getSize();

    const auto main = r.files.isEmpty() ? juce::File() : r.files.getFirst();
    text << "Saved " << main.getFileName() << " (" << juce::File::descriptionOfSizeInBytes (main.getSize()) << ")";

    if (r.files.size() > 1)
        text << "\n+ " << (r.files.size() - 1) << " stems in \"" << main.getFileNameWithoutExtension() << " - stems\"";

    if (r.missingAudio > 0)
        text << "\n\nNote: " << r.missingAudio << " audio file(s) hadn't arrived on this PC yet, so they're silent in it.";

    if (r.missingPlugins > 0)
        text << "\n\nNote: " << r.missingPlugins << " plugin(s) aren't installed on this PC, so they're silent / bypassed in it.";

    appendLog ("Exported " + main.getFullPathName() + " (" + juce::File::descriptionOfSizeInBytes (bytes) + " in total)");

    auto* w = new juce::AlertWindow ("Export finished", text, juce::MessageBoxIconType::InfoIcon);
    w->addButton ("Show in folder", 1);
    w->addButton ("OK", 0, juce::KeyPress (juce::KeyPress::returnKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([main] (int button)
    {
        if (button == 1 && main.existsAsFile())
            main.revealToUser();
    }), true);
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
            showMixer();
            appendLog ("Insert \"" + name + "\" records from " + inputs[r - 100].name
                        + " (you hear it through its effects). Add autotune/EQ to it if you like, then press Rec.");
        }

        refresh();
    });
}

//==============================================================================
// Song files
juce::File MainComponent::songsFolder()
{
    return appdata::projectsFolder();
}

juce::File MainComponent::autosaveFolder() const
{
    // A saved song keeps its backups in its own project folder; others go to Backups.
    if (currentFile != juce::File() && ! appdata::isBackup (currentFile))
        return appdata::projectFolderOf (currentFile).getChildFile ("Backups");

    return appdata::backupsFolder();
}

juce::int64 MainComponent::currentHash() const
{
    auto* sync = const_cast<collab::Session&> (session).getSync();
    return sync != nullptr ? sync->computeContentHash() : 0;
}

void MainComponent::updateTitle()
{
    auto* window = findParentComponentOfClass<juce::DocumentWindow>();

    if (window == nullptr)
        return;

    auto title = session.getSongTitle() + (dirty ? " *" : "");

    if (session.getMode() == collab::Session::Mode::joined)
        title << "  (the host's song)";

    title << "  -  SyncTest " << juce::JUCEApplication::getInstance()->getApplicationVersion();

    if (window->getName() != title)
        window->setName (title);
}

void MainComponent::showFileMenu()
{
    const bool canReplace = session.canReplaceSong();
    juce::PopupMenu m, recent;

    auto item = [] (int id, const juce::String& text, bool enabled, const juce::String& keys)
    {
        juce::PopupMenu::Item i (text);
        i.itemID = id;
        i.isEnabled = enabled;
        i.shortcutKeyDescription = keys;
        return i;
    };

    m.addItem (item (1, "New song", canReplace, "Ctrl+N"));
    m.addItem (item (2, "Open...", canReplace, "Ctrl+O"));

    recentFiles.createPopupMenuItems (recent, 100, false, true);

    if (recent.getNumItems() == 0)
        recent.addItem (-1, "(none yet)", false);

    m.addSubMenu ("Open recent", recent, canReplace);
    m.addSeparator();
    m.addItem (item (3, "Save", true, "Ctrl+S"));
    m.addItem (item (4, "Save as...", true, "Ctrl+Shift+S"));
    m.addSeparator();
    m.addItem (item (6, "Export audio (WAV, FLAC, MP3)...", ! exporter.isRunning(), "Ctrl+R"));
    m.addSeparator();
    m.addItem (5, "Open an automatic backup...", canReplace);
    m.addItem (7, currentFile != juce::File() ? "Show this project's folder" : "Show the Projects folder");

    juce::PopupMenu data;
    data.addItem (8, "Open " + appdata::getRoot().getFullPathName());
    data.addItem (9, "Use another data folder...");
    m.addSubMenu ("Data folder", data);

    if (! canReplace)
    {
        m.addSeparator();
        m.addItem (-1, "(only the host can open or start songs during a session)", false);
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (fileButton), [this] (int r)
    {
        if (r == 1)        newSong();
        else if (r == 2)   openSong();
        else if (r == 3)   saveSong (false);
        else if (r == 4)   saveSong (true);
        else if (r == 6)   showExportDialog();
        else if (r == 7)   (currentFile != juce::File() ? appdata::projectFolderOf (currentFile) : songsFolder()).startAsProcess();
        else if (r == 8)   appdata::getRoot().startAsProcess();
        else if (r == 9)   chooseDataFolder();
        else if (r == 5)
        {
            askToSaveChanges ([this]
            {
                autosaveFolder().createDirectory();
                fileChooser = std::make_unique<juce::FileChooser> ("Open an automatic backup", autosaveFolder(), appdata::songWildcards);
                fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                          [this] (const juce::FileChooser& fc)
                {
                    if (fc.getResult().existsAsFile())
                        openSongFile (fc.getResult());
                });
            });
        }
        else if (r >= 100)
        {
            const auto file = recentFiles.getFile (r - 100);
            askToSaveChanges ([this, file] { openSongFile (file); });
        }
    });
}

void MainComponent::chooseDataFolder()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Where COTOSTUDIO keeps everything (projects, backups, exports, settings)",
                                                       appdata::getRoot().getParentDirectory());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();

        if (! folder.isDirectory() || folder == appdata::getRoot())
            return;

        appdata::chooseRoot (folder);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Data folder",
            "From the next start COTOSTUDIO keeps everything in\n" + folder.getFullPathName()
             + "\n\nYour current files stay in " + appdata::getRoot().getFullPathName()
             + " - move the Projects folder over yourself if you want them there too.");
    });
}

void MainComponent::askToSaveChanges (std::function<void()> proceed)
{
    if (! dirty || session.getMode() == collab::Session::Mode::joined)
    {
        proceed();   // (a joiner's copy is the host's song: nothing of theirs to lose)
        return;
    }

    auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, "Save changes?",
                                                                    "Save the changes to \"" + session.getSongTitle() + "\" first?",
                                                                    "Save", "Don't save", "Cancel", this);
    juce::AlertWindow::showAsync (options, [safe = juce::Component::SafePointer<MainComponent> (this), proceed] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == 1)
            safe->saveSong (false, [proceed] (bool saved) { if (saved) proceed(); });
        else if (result == 2)
            proceed();
    });
}

void MainComponent::newSong()
{
    if (! session.canReplaceSong())
    {
        appendLog ("Only the host can start a new song during a session");
        return;
    }

    askToSaveChanges ([this]
    {
        const auto error = session.newProject();

        if (error.isNotEmpty())
        {
            appendLog (error);
            return;
        }

        currentFile = juce::File();
        savedHash = lastAutosaveHash = currentHash();
        dirty = false;
        state.viewedPattern = state.selectedChannel = state.placeAudioChannel = state.selectedInsert = {};
        appendLog ("New song");
        refresh();
    });
}

void MainComponent::openSong()
{
    if (! session.canReplaceSong())
    {
        appendLog ("Only the host can open a song during a session (you can still save your copy)");
        return;
    }

    askToSaveChanges ([this]
    {
        songsFolder().createDirectory();
        fileChooser = std::make_unique<juce::FileChooser> ("Open a song", currentFile.existsAsFile() ? currentFile : songsFolder(),
                                                           appdata::songWildcards);
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult().existsAsFile())
                openSongFile (fc.getResult());
        });
    });
}

void MainComponent::openSongFile (const juce::File& file)
{
    if (session.isBusyWithFile())
    {
        appendLog ("Still busy saving or opening the last song...");
        return;
    }

    appendLog ("Opening " + file.getFileName() + "...");
    expectingOwnSong = true;

    session.openProject (file, [safe = juce::Component::SafePointer<MainComponent> (this), file] (juce::String error)
    {
        if (safe == nullptr)
            return;

        auto& self = *safe;
        self.expectingOwnSong = false;

        if (error.isNotEmpty())
        {
            self.appendLog ("Could not open " + file.getFileName() + ": " + error);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not open the song", error);
            return;
        }

        self.currentFile = appdata::isBackup (file) ? juce::File() : file;   // backups: Save asks for a name
        self.savedHash = self.lastAutosaveHash = self.currentHash();
        self.dirty = false;
        self.state.viewedPattern = self.state.selectedChannel = self.state.placeAudioChannel = self.state.selectedInsert = {};

        if (self.currentFile != juce::File())
        {
            self.recentFiles.addFile (file);
            self.settings()->setValue ("recentSongs", self.recentFiles.toString());
        }

        self.refresh();
    });
}

void MainComponent::saveSong (bool saveAs, std::function<void (bool)> then)
{
    if (session.isBusyWithFile())
    {
        appendLog ("Still busy saving or opening the last song...");

        if (then)
            then (false);

        return;
    }

    if (! saveAs && currentFile != juce::File())
    {
        // Songs from older versions (.syncsong) are saved as a project from now on.
        writeSong (currentFile.hasFileExtension (appdata::songExtension)
                     ? currentFile : appdata::projectSongFile (currentFile.withFileExtension (appdata::songExtension)),
                   then);
        return;
    }

    // Save as: a name, and the song gets its own folder in Projects.
    auto* w = new juce::AlertWindow ("Save the song", "Name of the project (it gets its own folder in "
                                        + songsFolder().getFullPathName() + ")", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", session.getSongTitle());
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this), w, then] (int r)
    {
        if (safe == nullptr)
            return;

        const auto name = w->getTextEditorContents ("name").trim();

        if (r != 1 || name.isEmpty())
        {
            if (then)
                then (false);

            return;
        }

        const auto file = appdata::songFileForName (name);

        if (file.existsAsFile() && file != safe->currentFile)
        {
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Replace it?",
                                                                         "There is already a project called \"" + file.getFileNameWithoutExtension()
                                                                          + "\". Replace its song with this one?",
                                                                         "Replace", "Cancel", safe.getComponent());
            juce::AlertWindow::showAsync (options, [safe, file, then] (int result)
            {
                if (safe != nullptr && result == 1)
                    safe->writeSong (file, then);
                else if (then)
                    then (false);
            });
            return;
        }

        safe->writeSong (file, then);
    }), true);
}

void MainComponent::writeSong (const juce::File& file, std::function<void (bool)> then)
{
    // The file name is the song's name (everyone sees it in their title bar).
    const auto title = file.getFileNameWithoutExtension();

    if (auto* edit = state.getEdit(); edit != nullptr && song::getTitle (*edit) != title)
        song::setTitle (*edit, title);

    appendLog ("Saving \"" + title + "\"...");

    session.saveProject (file, true, [safe = juce::Component::SafePointer<MainComponent> (this), file, title, then]
                                     (project::SaveResult result)
    {
        if (safe == nullptr)
            return;

        auto& self = *safe;

        if (! result.ok())
        {
            self.appendLog ("Could not save: " + result.error);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not save the song", result.error);

            if (then)
                then (false);

            return;
        }

        self.currentFile = file;
        self.savedHash = result.songHash;
        self.dirty = self.currentHash() != self.savedHash;   // changes made while it was saving stay unsaved
        self.recentFiles.addFile (file);
        self.settings()->setValue ("recentSongs", self.recentFiles.toString());
        self.updateTitle();

        self.appendLog ("Saved \"" + title + "\" in " + file.getParentDirectory().getFullPathName()
                         + (result.audioIncluded > 0 ? " (" + juce::String (result.audioIncluded) + " audio files in Audio / Takes"
                                                         + (result.audioWritten > 0 ? ", " + juce::String (result.audioWritten) + " new)" : juce::String (")"))
                                                     : juce::String()));

        if (result.audioMissing > 0)
            self.appendLog (juce::String (result.audioMissing) + " audio file(s) hadn't arrived on this PC yet and aren't in it");

        if (then)
            then (true);
    });
}

void MainComponent::autosave()
{
    auto* edit = state.getEdit();

    if (edit == nullptr || session.isBusyWithFile() || session.isRecording())
        return;

    const auto hash = currentHash();

    if (hash == savedHash || hash == lastAutosaveHash)
        return;   // nothing new since the last save / backup

    auto folder = autosaveFolder();
    folder.createDirectory();
    const auto file = folder.getChildFile (juce::File::createLegalFileName (session.getSongTitle() + " (backup "
                                            + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ")")
                                            + appdata::songExtension);

    // Song only: its audio files stay in this PC's audio folder anyway.
    session.saveProject (file, false, [safe = juce::Component::SafePointer<MainComponent> (this), folder] (project::SaveResult result)
    {
        if (safe == nullptr)
            return;

        if (! result.ok())
        {
            safe->appendLog ("Automatic backup failed: " + result.error);
            return;
        }

        safe->lastAutosaveHash = result.songHash;

        // Keep the 10 newest backups.
        auto backups = folder.findChildFiles (juce::File::findFiles, false, appdata::songWildcards);
        std::sort (backups.begin(), backups.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getLastModificationTime() > b.getLastModificationTime(); });

        for (int i = 10; i < backups.size(); ++i)
            backups.getReference (i).deleteFile();
    });
}

void MainComponent::requestQuit()
{
    askToSaveChanges ([] { juce::JUCEApplication::getInstance()->quit(); });
}

void MainComponent::showPluginScanner()
{
    auto& pm = engine.getPluginManager();
    auto deadMansPedal = appdata::settingsFolder().getChildFile ("PluginScanCrashes.txt");

    auto* scanner = new juce::PluginListComponent (pm.pluginFormatManager, pm.knownPluginList, deadMansPedal,
                                                   settings(), true);
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
    fileButton.setBounds (top.removeFromLeft (54));
    top.removeFromLeft (6);
    soundsButton.setBounds (top.removeFromLeft (66));
    soundsButton.setToggleState (soundsVisible, juce::dontSendNotification);
    top.removeFromLeft (8);
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
    playingLabel.setBounds (top.removeFromRight (juce::jmin (260, top.getWidth() / 2)));   // what's playing
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
    bar.removeFromLeft (4);
    shareButton.setBounds (bar.removeFromLeft (92));
    focusButton.setBounds (bar.removeFromLeft (58));
    bar.removeFromLeft (6);
    playButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (4);
    recordButton.setBounds (bar.removeFromLeft (60));
    bar.removeFromLeft (4);
    clickButton.setBounds (bar.removeFromLeft (58));
    bar.removeFromLeft (8);
    tempoLabel.setBounds (bar.removeFromLeft (34));
    tempoSlider.setBounds (bar.removeFromLeft (100));
    bar.removeFromLeft (8);
    patternBox.setBounds (bar.removeFromLeft (120));
    bar.removeFromLeft (4);
    newPatternButton.setBounds (bar.removeFromLeft (28));
    bar.removeFromLeft (4);
    patternMenuButton.setBounds (bar.removeFromLeft (32));
    bar.removeFromLeft (6);
    lengthBox.setBounds (bar.removeFromLeft (72));

    r.removeFromTop (8);
    logView.setBounds (r.removeFromBottom (70));
    r.removeFromBottom (8);

    sounds.setVisible (soundsVisible);

    if (soundsVisible)
    {
        sounds.setBounds (r.removeFromLeft (SoundBrowser::defaultWidth));
        r.removeFromLeft (6);
    }

    browser.setVisible (browserVisible);
    showBrowserButton.setVisible (! browserVisible);

    if (browserVisible)
        browser.setBounds (r.removeFromLeft (Browser::defaultWidth));
    else
        showBrowserButton.setBounds (r.removeFromLeft (18));

    r.removeFromLeft (6);

    juce::Component* panels[] = { &playlist, &divider1, &rack, &divider2, &bottomTabs };
    panelLayout.layOutComponents (panels, 5, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
}

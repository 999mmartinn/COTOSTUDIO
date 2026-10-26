#include "MainComponent.h"
#include "PatternCompiler.h"
#include "ProjectFile.h"
#include "AppData.h"

static const juce::Colour background = theme::colours::bg, accent = theme::colours::accent;

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
/** A panel (mixer, piano roll, channel rack) in a window of its own - e.g. full
    screen on a second monitor. Closing it puts the panel back in the main window. */
class MainComponent::PanelWindow  : public juce::DocumentWindow
{
public:
    PanelWindow (MainComponent& o, Panel p)
        : DocumentWindow (o.panelName (p) + "  -  COTOSTUDIO", theme::colours::bg, DocumentWindow::allButtons),
          owner (o), panel (p)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (480, 220, 8000, 5000);
        setContentNonOwned (&owner.panelComponent (panel), false);

        const auto saved = owner.settings()->getValue (owner.panelKey (panel) + "Window");

        if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
        {
            // First time: on the second screen if there is one, otherwise over the main window.
            auto& displays = juce::Desktop::getInstance().getDisplays();
            auto area = displays.getPrimaryDisplay() != nullptr ? displays.getPrimaryDisplay()->userArea : juce::Rectangle<int> (0, 0, 1280, 800);

            for (auto& d : displays.displays)
                if (! d.isMain)
                    area = d.userArea;

            const auto size = panel == Panel::rack ? juce::Point<int> (1100, 600)
                            : panel == Panel::pianoRoll ? juce::Point<int> (1500, 820)
                                                        : juce::Point<int> (1400, 700);
            setBounds (area.withSizeKeepingCentre (juce::jmin (size.x, area.getWidth() - 80), juce::jmin (size.y, area.getHeight() - 80)));
        }

        setVisible (true);
        ready = true;
    }

    ~PanelWindow() override
    {
        clearContentComponent();   // the panel belongs to the main window
    }

    void closeButtonPressed() override                        { owner.setPanelDetached (panel, false); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey && isFullScreen())
        {
            setFullScreen (false);
            return true;
        }

        return owner.keyPressed (key);   // Space, Ctrl+S... work here too
    }

    void moved() override                                     { DocumentWindow::moved(); remember(); }

    void resized() override
    {
        DocumentWindow::resized();
        remember();

        if (ready)
            owner.updateFullScreenButtons();
    }

private:
    MainComponent& owner;
    const Panel panel;
    bool ready = false;

    void remember()
    {
        if (ready)
            owner.settings()->setValue (owner.panelKey (panel) + "Window", getWindowStateAsString());
    }
};

/** What a detached panel's place in the main window shows meanwhile: a note and
    "Show the window" / "Bring it back here". The rack's is a slim strip. */
class MainComponent::PanelPlaceholder  : public juce::Component
{
public:
    PanelPlaceholder (MainComponent& o, Panel p) : owner (o), panel (p)
    {
        theme::setStyle (showButton, "accent");
        theme::setIcon (backButton, theme::Icon::attach);
        backButton.onClick = [this] { owner.setPanelDetached (panel, false); };
        showButton.onClick = [this] { owner.showPanel (panel); };
        addAndMakeVisible (backButton);
        addAndMakeVisible (showButton);
    }

    bool isStrip() const        { return panel == Panel::rack; }

    juce::String message() const
    {
        return panel == Panel::rack      ? TRANS ("The channel rack is in its own window.")
             : panel == Panel::pianoRoll ? TRANS ("The piano roll is in its own window.")
                                         : TRANS ("The mixer is in its own window.");
    }

    void paint (juce::Graphics& g) override
    {
        if (isStrip())
        {
            auto r = getLocalBounds().withTrimmedBottom (6);
            theme::fillPanel (g, r);
            r.reduce (14, 0);
            theme::drawIcon (g, theme::Icon::detach, r.removeFromLeft (18).toFloat().withSizeKeepingCentre (16.0f, 16.0f),
                             theme::colours::accent, 1.6f);
            r.removeFromLeft (10);
            g.setColour (theme::colours::muted);
            g.setFont (theme::ui (12.5f, theme::Weight::medium));
            g.drawText (message(), r.withRight (showButton.getX() - 10), juce::Justification::centredLeft, true);
            return;
        }

        // The icon, the note under it, then the buttons (see resized).
        g.fillAll (theme::colours::wellAlt);
        const int cy = getHeight() / 2;
        theme::drawIcon (g, theme::Icon::detach, juce::Rectangle<float> (22.0f, 22.0f).withCentre ({ getWidth() * 0.5f, (float) cy - 52.0f }),
                         theme::colours::accent, 1.8f);
        g.setColour (theme::colours::muted);
        g.setFont (theme::ui (13.5f, theme::Weight::medium));
        g.drawFittedText (message(), getLocalBounds().withY (cy - 34).withHeight (22), juce::Justification::centred, 1);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (isStrip())
            theme::framePanel (g, getLocalBounds().withTrimmedBottom (6));
    }

    void resized() override
    {
        if (isStrip())
        {
            auto r = getLocalBounds().withTrimmedBottom (6).reduced (10, 0);
            r = r.withSizeKeepingCentre (r.getWidth(), 28);
            backButton.setBounds (r.removeFromRight (170));
            r.removeFromRight (8);
            showButton.setBounds (r.removeFromRight (160));
            return;
        }

        auto r = getLocalBounds().withTrimmedTop (getHeight() / 2).removeFromTop (30).withSizeKeepingCentre (380, 30);
        showButton.setBounds (r.removeFromLeft (170));
        r.removeFromLeft (10);
        backButton.setBounds (r);
    }

private:
    MainComponent& owner;
    const Panel panel;
    juce::TextButton backButton { TRANS ("Bring it back here") }, showButton { TRANS ("Show the window") };
};

//==============================================================================
/** The logo at the top left (click: About). */
class MainComponent::Logo  : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    explicit Logo (MainComponent& o) : owner (o), image (theme::logoBadge())
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("COTOSTUDIO " + juce::JUCEApplication::getInstance()->getApplicationVersion() + "  -  " + TRANS ("About"));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();

        if (image.isValid())
        {
            // The badge, scaled to fit, with rounded corners.
            const auto fit = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                                 .appliedTo (image.getBounds().toFloat(), r);
            juce::Path clip;
            clip.addRoundedRectangle (fit, 7.0f);
            g.saveState();
            g.reduceClipRegion (clip);
            g.drawImage (image, fit, juce::RectanglePlacement::stretchToFit);
            g.restoreState();
        }
        else
        {
            g.setColour (theme::colours::accent);
            g.setFont (theme::ui (16.0f, theme::Weight::bold));
            g.drawText ("COTOSTUDIO", getLocalBounds(), juce::Justification::centredLeft, false);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()))
            owner.showAbout();
    }

private:
    MainComponent& owner;
    juce::Image image;
};

/** The rounded panel around the Piano roll / Mixer / Automation tabs. */
class MainComponent::Dock  : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (theme::colours::wellAlt);
        g.fillRoundedRectangle (r, 8.0f);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (theme::colours::line);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f, 1.0f);
    }

    void resized() override
    {
        if (auto* tabs = getChildComponent (0))
            tabs->setBounds (getLocalBounds().reduced (1));
    }
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
    session.setMetronome (prefs->getBoolValue ("metronome", false));
    session.setCountInBars (prefs->getIntValue ("countInBars", 0));

    logo = std::make_unique<Logo> (*this);
    dock = std::make_unique<Dock>();
    dock->addAndMakeVisible (bottomTabs);

    for (auto* c : std::initializer_list<juce::Component*> {
             logo.get(), &fileButton, &soundsButton, &sessionPill, &lcd, &settingsButton,
             &patModeButton, &songModeButton, &shareButton, &focusButton, &playButton, &recordButton, &clickButton, &tempoSlider, &patternBox, &newPatternButton, &patternMenuButton,
             &lengthBox, &undoButton, &redoButton,
             &sounds, &browser, &showBrowserButton, &playlist, &divider1, &rack, &divider2, &dividerNoRack, dock.get(), &statusBar })
        addAndMakeVisible (c);

    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setFont (theme::mono (12.5f));
    logView.setIndents (10, 8);
    logView.setColour (juce::TextEditor::backgroundColourId, theme::colours::well);

    // Top bar look: icons, styles, help texts.
    theme::setIcon (fileButton, theme::Icon::chevronDown);
    fileButton.getProperties().set ("iconRight", true);
    fileButton.setButtonText (TRANS ("File"));
    theme::setIcon (soundsButton, theme::Icon::sounds);
    soundsButton.setButtonText (TRANS ("Sounds"));
    theme::setIcon (playButton, theme::Icon::play);
    theme::setStyle (playButton, "play");
    playButton.setTooltip (TRANS ("Play / stop (Space). Play starts at the green marker; Stop goes back to it"));
    theme::setIcon (recordButton, theme::Icon::record);
    theme::setStyle (recordButton, "record");
    theme::setIcon (clickButton, theme::Icon::metronome);
    theme::setIcon (shareButton, theme::Icon::share);
    theme::setIcon (focusButton, theme::Icon::focus);
    theme::setIcon (undoButton, theme::Icon::undo);
    theme::setIcon (redoButton, theme::Icon::redo);
    theme::setIcon (settingsButton, theme::Icon::gear);
    theme::setIcon (newPatternButton, theme::Icon::plus);
    theme::setIcon (patternMenuButton, theme::Icon::menu);
    theme::setIcon (showBrowserButton, theme::Icon::chevronRight);
    theme::setStyle (showBrowserButton, "flat");
    showBrowserButton.setButtonText ({});
    settingsButton.setTooltip (TRANS ("Settings: sound card, plugins, language, data folder"));
    settingsButton.onClick = [this] { showSettingsMenu(); };
    sessionPill.onClick = [this] { showSessionPanel(); };
    statusBar.onShowLog = [this] { showLog(); };
    patternBox.setTooltip (TRANS ("The pattern you're editing (piano roll and channel rack)"));

    // The tempo lives in the counter.
    tempoSlider.setColour (juce::Slider::textBoxTextColourId, theme::colours::lcdText);
    tempoSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    tempoSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    tempoSlider.setColour (juce::TextButton::textColourOffId, theme::colours::lcdText);

    tempoSlider.setRange (40.0, 250.0, 1.0);
    tempoSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 46, 22);
    tempoSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
    tempoSlider.setTooltip (TRANS ("Tempo. In a session everyone else has to accept a change (it is part of the song)"));
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

    lengthBox.addItem (TRANS ("1 bar"), 1);
    lengthBox.addItem (TRANS ("2 bars"), 2);
    lengthBox.addItem (TRANS ("4 bars"), 4);
    lengthBox.addItem (TRANS ("8 bars"), 8);
    lengthBox.setTooltip (TRANS ("Length of the viewed pattern"));
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

    newPatternButton.setTooltip (TRANS ("New pattern"));
    newPatternButton.onClick = [this]
    {
        if (auto* edit = state.getEdit())
        {
            state.beginAction();
            auto p = song::addPattern (*edit, song::nextPatternName (*edit), song::getBars (state.getPattern()));
            selectPattern (p[song::ids::sid].toString());
        }
    };

    patternMenuButton.setTooltip (TRANS ("Rename / clone / delete pattern"));
    patternMenuButton.onClick = [this] { showPatternMenu(); };

    state.onSelectionChanged = [this] { refresh(); };

    // Browser (left): patterns, audio files, people + chat.
    browser.onPatternChosen = [this] (const juce::String& sid) { selectPattern (sid); };
    browser.onCollapse = [this] { setBrowserVisible (false); };
    browser.onImportAudio = [this] { playlist.showImportDialog(); };
    browser.onUserNameChanged = [this] (const juce::String& name) { settings()->setValue ("userName", name); };
    showBrowserButton.setTooltip (TRANS ("Show the browser (F8)"));
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
    soundsButton.setTooltip (TRANS ("Your folders of sounds and MIDI (Alt+F8). Only on this PC until you drag a sound into the song"));
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

    fileButton.setTooltip (TRANS ("New, open and save songs (Ctrl+N / Ctrl+O / Ctrl+S)"));
    fileButton.onClick = [this] { showFileMenu(); };

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

    recordButton.setTooltip (TRANS ("Record (R): records every armed mixer insert from the song start marker (or the punch range). Press Stop or Space to finish. Right-click: recording options (count-in, punch in/out, disarm all...)"));

    clickButton.setClickingTogglesState (false);
    clickButton.setTooltip (TRANS ("Metronome on/off (only you hear it). Right-click: count-in before recording"));
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
        {
            const auto& punch = session.getRecorder().getPunch();
            auto bar = [] (double beat) { return juce::String (beat / 4.0 + 1.0, std::abs (std::fmod (beat, 4.0)) < 0.001 ? 0 : 2); };

            if (punch.isActive())
                appendLog (TRANS ("Punch recording bars {FROM} to {TO}: the song plays from bar {START}; recording stops by itself at the punch-out")
                             .replace ("{FROM}", bar (punch.inBeat)).replace ("{TO}", bar (punch.outBeat))
                             .replace ("{START}", bar (juce::jmax (0.0, punch.inBeat - 4.0 * punch.preRollBars))));
            else
                appendLog (TRANS ("Recording... press Stop or Space to finish"));
        }

        refresh();
    };

    const auto tabColour = theme::colours::wellAlt;
    bottomTabs.addTab (TRANS ("Piano roll"), tabColour, &pianoRoll, false);
    bottomTabs.addTab (TRANS ("Mixer"), tabColour, &mixer, false);
    bottomTabs.addTab (TRANS ("Automation"), tabColour, &automationEditor, false);

    state.createAutomation = [this] (const juce::String& pluginID, const juce::String& paramID) { createAutomation (pluginID, paramID); };
    state.onAutomationSelected = [this]
    {
        bottomTabs.setCurrentTabIndex (2);
        refresh();
    };
    bottomTabs.setTabBarDepth (32);
    bottomTabs.setOutline (0);
    bottomTabs.setIndent (0);

    // The mixer, piano roll and channel rack can each have a window of their own
    // (e.g. full screen on a second monitor); the playlist and the piano roll can
    // also fill the main window.
    for (int i = 0; i < numPanels; ++i)
        placeholders[i] = std::make_unique<PanelPlaceholder> (*this, (Panel) i);

    mixer.onDetachToggle    = [this] { setPanelDetached (Panel::mixer, ! isDetached (Panel::mixer)); };
    mixer.onFullScreenToggle = [this] { togglePanelFullScreen (Panel::mixer); };
    pianoRoll.onDetachToggle = [this] { setPanelDetached (Panel::pianoRoll, ! isDetached (Panel::pianoRoll)); };
    pianoRoll.onFullScreenToggle = [this] { togglePanelFullScreen (Panel::pianoRoll); };
    rack.onDetachToggle     = [this] { setPanelDetached (Panel::rack, ! isDetached (Panel::rack)); };
    rack.onFullScreenToggle = [this] { togglePanelFullScreen (Panel::rack); };
    playlist.onFullScreenToggle = [this]
    {
        setMaximised (maximised == Maximised::playlist ? Maximised::none : Maximised::playlist);
    };

    // The scale helper is remembered (this PC only).
    state.scaleSnap = prefs->getBoolValue ("scaleSnap", false);
    state.scaleKey = juce::jlimit (0, 11, prefs->getIntValue ("scaleKey", 0));
    state.scaleType = prefs->getIntValue ("scaleType", 1);
    pianoRoll.onScaleChanged = [this]
    {
        settings()->setValue ("scaleSnap", state.scaleSnap);
        settings()->setValue ("scaleKey", state.scaleKey);
        settings()->setValue ("scaleType", state.scaleType);
    };

    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr)
            for (int i = 0; i < numPanels; ++i)
                if (safe->settings()->getBoolValue (panelKey ((Panel) i) + "Detached", false))
                    safe->setPanelDetached ((Panel) i, true);
    });

    // Panels: playlist, divider, rack, divider, piano roll / mixer tabs (sizes are proportions or pixels).
    panelLayout.setItemLayout (0, 110, -0.7, -0.30);
    panelLayout.setItemLayout (1, 6, 6, 6);
    panelLayout.setItemLayout (2, 110, -0.6, -0.31);
    panelLayout.setItemLayout (3, 6, 6, 6);
    panelLayout.setItemLayout (4, 120, -0.8, -0.39);

    panelLayoutNoRack.setItemLayout (0, 110, -0.8, -0.45);
    panelLayoutNoRack.setItemLayout (1, 6, 6, 6);
    panelLayoutNoRack.setItemLayout (2, 50, 50, 50);
    panelLayoutNoRack.setItemLayout (3, 120, -0.85, -0.55);

    for (auto* b : { &patModeButton, &songModeButton })
    {
        b->setClickingTogglesState (false);
        theme::setStyle (*b, "toggle");
    }

    patModeButton.setButtonText (TRANS ("PAT"));
    songModeButton.setButtonText (TRANS ("SONG"));
    patModeButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    songModeButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    patModeButton.setTooltip (TRANS ("Pattern mode: Play loops the pattern you are editing"));
    songModeButton.setTooltip (TRANS ("Song mode: Play plays the playlist"));
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

    focusButton.setButtonText (TRANS ("Focus"));
    focusButton.setTooltip (TRANS ("Focus: when someone shares their playback, you keep working on your own and aren't pulled in"));
    focusButton.onClick = [this] { session.setFocus (! session.isFocus()); refresh(); };

    undoButton.onClick = [this] { undo (false); };
    redoButton.onClick = [this] { undo (true); };
    undoButton.setTooltip (TRANS ("Undo your last change (Ctrl+Z)"));
    redoButton.setTooltip (TRANS ("Redo (Ctrl+Y)"));

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

    // Recording: your inputs (which input feeds which insert, armed or not), monitoring
    // and punch settings are remembered on this PC.
    {
        auto& rec = session.getRecorder();
        rec.setMonitoring (prefs->getBoolValue ("monitorInputs", true));
        rec.restoreChoices (lastRecordChoices = prefs->getValue ("recordInputs"));
        lastRecordChoices = rec.saveChoices();
        Recorder::Punch punch;
        punch.preRollBars = prefs->getIntValue ("punchPreRoll", 1);
        rec.setPunch (punch);
    }

    playlist.onPunchChanged = [this] { refresh(); };

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

    appendLog ("COTOSTUDIO " + juce::JUCEApplication::getInstance()->getApplicationVersion() + " - "
                + TRANS ("first time? Settings (gear, top right) > Find my plugins, so your VST3 plugins can be used"));
    setSize (1600, 940);
    startTimerHz (5);
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveSoundsState();
    settings()->setValue ("recordInputs", session.getRecorder().saveChoices());

    for (auto& w : panelWindows)
        w.reset();

    logWindow.reset();   // shows logView, which goes first otherwise
    settings()->setValue ("previewVolume", (double) previewer.getGain());
    session.clearCallbacks();   // the session goes after the window's parts: it must not call into them
    unwatchEdit();
}

void MainComponent::appendLog (const juce::String& s)
{
    logView.moveCaretToEnd();
    logView.insertTextAtCaret (juce::Time::getCurrentTime().toString (false, true, true, true) + "  " + s + "\n");
    statusBar.setMessage (s);
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
    const auto what = session.isSongMode() ? TRANS ("Song") : playingPattern[song::ids::name].toString();
    const auto whose = session.isFollowingShare() ? session.getShareOwnerName() + ": " : juce::String();
    statusBar.setPlaying (playing ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + whose + what
                                      + (session.isSharing() ? "  (" + TRANS ("shared") + ")" : juce::String())
                                  : juce::String());

    patModeButton.setToggleState (! session.isSongMode(), juce::dontSendNotification);
    songModeButton.setToggleState (session.isSongMode(), juce::dontSendNotification);
}

void MainComponent::refresh()
{
    using Mode = collab::Session::Mode;
    auto mode = session.getMode();

    sessionPill.repaint();
    lcd.refresh();

    if (auto* edit = session.getEdit())
    {
        theme::setIcon (playButton, edit->getTransport().isPlaying() ? theme::Icon::stop : theme::Icon::play);

        const bool recording = session.isRecording();
        recordButton.setToggleState (recording, juce::dontSendNotification);   // red while recording

        refreshTempo();

        undoButton.setEnabled (edit->getUndoManager().canUndo());
        redoButton.setEnabled (edit->getUndoManager().canRedo());
    }

    // Sharing / Focus
    {
        const bool inSession = mode == Mode::hosting || mode == Mode::joined;
        const auto owner = session.getShareOwnerName().substring (0, 14);
        juce::Colour on = accent;

        if (session.isSharing())
        {
            shareButton.setButtonText (TRANS ("Sharing"));
            shareButton.setTooltip (TRANS ("Everyone following hears your playback, in sync. Only you control it. Click to stop sharing"));
        }
        else if (session.isFollowingShare())
        {
            on = session.getShareOwnerColour();
            shareButton.setButtonText (TRANS ("With") + " " + owner);
            shareButton.setTooltip (TRANS ("{NAME} controls the playback. Click to leave (you go back to where you were; the others keep listening)")
                                      .replace ("{NAME}", session.getShareOwnerName()));
        }
        else if (session.isShareRunning())
        {
            shareButton.setButtonText (TRANS ("Join") + " " + owner);
            shareButton.setTooltip (TRANS ("{NAME} is sharing their playback. Click to listen along").replace ("{NAME}", session.getShareOwnerName()));
        }
        else
        {
            shareButton.setButtonText (TRANS ("Share"));
            shareButton.setTooltip (inSession ? TRANS ("Share your playback: everyone (except people in Focus) hears what you play, in sync, and only you control it. Click again to stop")
                                              : TRANS ("Share your playback with the others (in a session)"));
        }

        shareButton.setColour (juce::TextButton::buttonOnColourId, on);
        shareButton.setColour (juce::TextButton::textColourOnId, theme::colours::accentText);
        shareButton.setToggleState (session.isSharing() || session.isFollowingShare(), juce::dontSendNotification);
        shareButton.setEnabled (inSession);
        focusButton.setToggleState (session.isFocus(), juce::dontSendNotification);
        clickButton.setToggleState (session.isMetronomeOn(), juce::dontSendNotification);
        clickButton.setButtonText (session.getCountInBars() > 0 ? TRANS ("Click") + " +" + juce::String (session.getCountInBars())
                                                                : TRANS ("Click"));

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
        appendLog (TRANS ("{NAME} controls the playback right now (click 'With {NAME}' to leave and play on your own)")
                     .replace ("{NAME}", session.getShareOwnerName()));
        return true;
    }

    if (key == juce::KeyPress::spaceKey)                                            { playButton.triggerClick(); return true; }
    if (key == juce::KeyPress ('r'))                                                { recordButton.triggerClick(); return true; }
    if (key == juce::KeyPress::F6Key)                                               { showPanel (Panel::rack); return true; }
    if (key == juce::KeyPress::F7Key)                                               { showPanel (Panel::pianoRoll); return true; }
    if (key == juce::KeyPress::F9Key)                                               { showMixer(); return true; }
    if (key == juce::KeyPress::escapeKey && maximised != Maximised::none)           { setMaximised (Maximised::none); return true; }
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
    m.addItem (1, TRANS ("Rename..."));
    m.addItem (2, TRANS ("Clone"));
    m.addItem (4, TRANS ("Split by channel"), song::getPatternChannels (*edit, pattern).size() >= 2);
    m.addItem (3, TRANS ("Delete"), song::getPatterns (*edit).size() > 1);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (patternMenuButton), [this, pattern] (int r)
    {
        auto* e = state.getEdit();

        if (e == nullptr || r == 0 || ! pattern.getParent().isValid())
            return;

        if (r == 1)
        {
            auto* w = new juce::AlertWindow (TRANS ("Rename pattern"), {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", pattern[song::ids::name].toString());
            w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

juce::Component& MainComponent::panelComponent (Panel p)
{
    switch (p)
    {
        case Panel::pianoRoll:  return pianoRoll;
        case Panel::rack:       return rack;
        case Panel::mixer:      break;
    }

    return mixer;
}

juce::String MainComponent::panelName (Panel p) const
{
    return p == Panel::pianoRoll ? TRANS ("Piano roll") : p == Panel::rack ? TRANS ("Channel rack") : TRANS ("Mixer");
}

juce::String MainComponent::panelKey (Panel p)
{
    return p == Panel::pianoRoll ? "pianoRoll" : p == Panel::rack ? "rack" : "mixer";
}

void MainComponent::showPanel (Panel p)
{
    if (auto& w = panelWindows[(int) p])
    {
        w->toFront (true);
        return;
    }

    if (p == Panel::pianoRoll)  bottomTabs.setCurrentTabIndex (0);
    if (p == Panel::mixer)      bottomTabs.setCurrentTabIndex (1);

    // A panel hidden by the other one filling the window comes back.
    if ((p == Panel::rack && maximised != Maximised::none)
         || (p != Panel::rack && maximised == Maximised::playlist))
        setMaximised (Maximised::none);
}

void MainComponent::setPanelDetached (Panel p, bool detached)
{
    const int i = (int) p;

    if (detached == isDetached (p))
        return;

    if (detached && p == Panel::pianoRoll && maximised == Maximised::pianoRoll)
        setMaximised (Maximised::none);

    // The panel's tab (piano roll 0, mixer 1) keeps its place, showing a note meanwhile.
    const int tab = p == Panel::pianoRoll ? 0 : p == Panel::mixer ? 1 : -1;
    const auto colour = tab >= 0 ? bottomTabs.getTabBackgroundColour (tab) : juce::Colour();
    const bool wasCurrent = tab >= 0 && bottomTabs.getCurrentTabIndex() == tab;

    if (detached)
    {
        if (tab >= 0)
        {
            bottomTabs.removeTab (tab);
            bottomTabs.addTab (panelName (p), colour, placeholders[i].get(), false, tab);

            if (wasCurrent)
                bottomTabs.setCurrentTabIndex (tab);
        }

        panelWindows[i] = std::make_unique<PanelWindow> (*this, p);
        appendLog (TRANS ("{PANEL} has its own window now: move it to your other screen").replace ("{PANEL}", panelName (p))
                     + (p == Panel::rack ? " (F6)" : p == Panel::pianoRoll ? " (F7)" : " (F9)"));
    }
    else
    {
        panelWindows[i].reset();

        if (tab >= 0)
        {
            bottomTabs.removeTab (tab);
            bottomTabs.addTab (panelName (p), colour, &panelComponent (p), false, tab);
            bottomTabs.setCurrentTabIndex (tab);
        }
        else
        {
            addAndMakeVisible (rack);   // back from its window
        }
    }

    if (p == Panel::mixer)          mixer.setDetached (detached);
    if (p == Panel::pianoRoll)      pianoRoll.setDetached (detached);
    if (p == Panel::rack)           rack.setDetached (detached);

    settings()->setValue (panelKey (p) + "Detached", detached);
    resized();
    updateFullScreenButtons();
}

void MainComponent::togglePanelFullScreen (Panel p)
{
    if (auto& w = panelWindows[(int) p])
    {
        w->setFullScreen (! w->isFullScreen());
        w->toFront (true);
    }
    else if (p == Panel::pianoRoll)
    {
        setMaximised (maximised == Maximised::pianoRoll ? Maximised::none : Maximised::pianoRoll);
    }

    updateFullScreenButtons();
}

void MainComponent::setMaximised (Maximised m)
{
    if (m == Maximised::pianoRoll && isDetached (Panel::pianoRoll))
        m = Maximised::none;

    maximised = m;

    if (m == Maximised::pianoRoll)
        bottomTabs.setCurrentTabIndex (0);

    resized();
    updateFullScreenButtons();
}

void MainComponent::updateFullScreenButtons()
{
    auto windowFull = [this] (Panel p) { auto& w = panelWindows[(int) p]; return w != nullptr && w->isFullScreen(); };

    playlist.setFullScreenState (maximised == Maximised::playlist);
    pianoRoll.setFullScreenState (isDetached (Panel::pianoRoll) ? windowFull (Panel::pianoRoll) : maximised == Maximised::pianoRoll);
    rack.setFullScreenState (windowFull (Panel::rack));
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

    // Which input feeds which insert (and armed or not), every 2 s if it changed.
    if (ticksSinceSoundsSave == 0)
    {
        const auto choices = session.getRecorder().saveChoices();

        if (choices != lastRecordChoices)
        {
            lastRecordChoices = choices;
            settings()->setValue ("recordInputs", choices);
        }
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

    auto* w = new juce::AlertWindow (TRANS ("Go to the shared pattern?"),
                                     TRANS ("{NAME} is playing \"{PATTERN}\".\nGo to that pattern? (If you stay, you keep working on yours and still hear theirs.)")
                                       .replace ("{NAME}", owner).replace ("{PATTERN}", pattern[song::ids::name].toString()),
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addButton (TRANS ("Go there"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (TRANS ("Stay here"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

    auto* w = new juce::AlertWindow (TRANS ("Change the tempo?"),
                                     TRANS ("{NAME} wants to change the tempo from {OLD} to {NEW} BPM.\nEveryone has to agree (30 seconds to answer).")
                                       .replace ("{NAME}", q.fromName).replace ("{OLD}", bpm (q.oldBpm)).replace ("{NEW}", bpm (q.bpm)),
                                     juce::MessageBoxIconType::QuestionIcon);
    w->addButton (TRANS ("Accept"), 1);
    w->addButton (TRANS ("Decline"), 0);
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
    tempoSlider.setColour (juce::Slider::textBoxTextColourId, waiting ? theme::colours::amber : theme::colours::lcdText);
    tempoSlider.setTooltip (pending != 0.0 ? TRANS ("Waiting for everyone to accept {NEW} BPM (the song is still at {OLD})")
                                                 .replace ("{NEW}", juce::String (pending, 0)).replace ("{OLD}", juce::String (tempo->getBpm(), 0))
                                           : TRANS ("Tempo. In a session everyone else has to accept a change (it is part of the song)"));
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
    appendLog (TRANS ("Automation clip for {KNOB}: shape it in the Automation tab. It plays in SONG mode, for everyone")
                 .replace ("{KNOB}", song::describeParameter (*edit, pluginID, paramID)));
    refresh();
}

void MainComponent::showRecordOptionsMenu (juce::Component* target)
{
    auto& rec = session.getRecorder();
    const auto punch = rec.getPunch();
    const int bars = session.getCountInBars();
    auto bar = [] (double beat) { return juce::String (beat / 4.0 + 1.0, std::abs (std::fmod (beat, 4.0)) < 0.001 ? 0 : 2); };

    juce::PopupMenu m, countIn, punchFrom, preRoll;
    m.addSectionHeader (TRANS ("Recording options"));

    countIn.addItem (1, TRANS ("None"), true, bars == 0);
    countIn.addItem (2, TRANS ("1 bar"), true, bars == 1);
    countIn.addItem (3, TRANS ("2 bars"), true, bars == 2);
    m.addSubMenu (TRANS ("Count-in before recording") + (bars > 0 ? ": " + juce::String (bars) : juce::String()), countIn);
    m.addItem (4, TRANS ("Metronome while playing"), true, session.isMetronomeOn());

    // Punch in / out
    m.addSeparator();
    const auto barsLong = juce::String ((punch.outBeat - punch.inBeat) / 4.0, std::abs (std::fmod (punch.outBeat - punch.inBeat, 4.0)) < 0.001 ? 0 : 2);
    m.addItem (10, punch.hasRange() ? TRANS ("Punch in / out: bars {FROM} to {TO}").replace ("{FROM}", bar (punch.inBeat)).replace ("{TO}", bar (punch.outBeat))
                                        + "  (" + TRANS ("{N} bar(s)").replace ("{N}", barsLong) + ")"
                                    : TRANS ("Punch in / out (set the range first)"),
               punch.hasRange(), punch.isActive());

    for (int n : { 1, 2, 4, 8 })
        punchFrom.addItem (10 + n, TRANS ("{N} bar(s)").replace ("{N}", juce::String (n)));

    m.addSubMenu (TRANS ("Punch range from the start marker (bar {BAR})").replace ("{BAR}", bar (state.songStartBeat)), punchFrom);
    m.addItem (19, TRANS ("Clear the punch range"), punch.hasRange());

    for (int n = 0; n <= 2; ++n)
        preRoll.addItem (30 + n, n == 0 ? TRANS ("None") : TRANS ("{N} bar(s)").replace ("{N}", juce::String (n)), true, punch.preRollBars == n);

    m.addSubMenu (TRANS ("Pre-roll before the punch-in") + ": " + (punch.preRollBars == 0 ? TRANS ("None")
                                                                                            : TRANS ("{N} bar(s)").replace ("{N}", juce::String (punch.preRollBars))), preRoll);
    m.addItem (-1, TRANS ("Tip: Ctrl + drag on the playlist ruler draws the punch range"), false);

    // Inputs
    m.addSeparator();
    const int armed = rec.getNumArmed();
    const auto disarmed = rec.getDisarmedWithInput();
    m.addItem (40, TRANS ("Disarm all") + (armed > 0 ? " (" + juce::String (armed) + ")" : juce::String()), armed > 0);
    m.addItem (41, TRANS ("Arm every insert that has an input") + (! disarmed.isEmpty() ? " (" + juce::String (disarmed.size()) + ")" : juce::String()),
               ! disarmed.isEmpty());
    m.addItem (42, TRANS ("Hear the inputs through their inserts (monitoring)"), true, rec.isMonitoring());
    m.addItem (43, TRANS ("Sound card and MIDI..."));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [this] (int r)
    {
        auto& recorder = session.getRecorder();
        auto p = recorder.getPunch();

        if (r >= 1 && r <= 3)
        {
            session.setCountInBars (r - 1);
            settings()->setValue ("countInBars", r - 1);
            appendLog (r == 1 ? TRANS ("Count-in off: recording starts right away")
                              : TRANS ("Count-in: {N} bar(s) of metronome before the take (it still starts at the marker)").replace ("{N}", juce::String (r - 1)));
        }
        else if (r == 4)
        {
            session.setMetronome (! session.isMetronomeOn());
            settings()->setValue ("metronome", session.isMetronomeOn());
        }
        else if (r == 10)
        {
            p.on = ! p.on;
            recorder.setPunch (p);
            appendLog (p.on ? TRANS ("Punch in / out on: Rec keeps only the punch range") : TRANS ("Punch in / out off: Rec records from the start marker"));
        }
        else if (r >= 11 && r <= 18)
        {
            setPunchFromStartMarker (r - 10);
        }
        else if (r == 19)
        {
            const int preRollBars = p.preRollBars;
            p = {};
            p.preRollBars = preRollBars;
            recorder.setPunch (p);
        }
        else if (r >= 30 && r <= 32)
        {
            p.preRollBars = r - 30;
            recorder.setPunch (p);
            savePunchSettings();
        }
        else if (r == 40)
        {
            recorder.disarmAll();
            appendLog (TRANS ("Every insert disarmed (their inputs stay, so you still hear them)"));
        }
        else if (r == 41)
        {
            recorder.armAllWithInput();
        }
        else if (r == 42)
        {
            recorder.setMonitoring (! recorder.isMonitoring());
            settings()->setValue ("monitorInputs", recorder.isMonitoring());
            appendLog (recorder.isMonitoring() ? TRANS ("Monitoring on: you hear your inputs through their inserts' effects")
                                               : TRANS ("Monitoring off: use your interface's direct monitor to hear yourself"));
        }
        else if (r == 43)
        {
            showAudioSettings();
        }

        playlist.repaint();
        refresh();
    });
}

void MainComponent::setPunchFromStartMarker (int bars)
{
    auto& recorder = session.getRecorder();
    auto p = recorder.getPunch();
    p.inBeat = std::floor (state.songStartBeat / 4.0 + 1.0e-6) * 4.0;
    p.outBeat = p.inBeat + 4.0 * bars;
    p.on = true;
    recorder.setPunch (p);
    appendLog (TRANS ("Punch in / out: bars {FROM} to {TO}. Rec plays from a bar before and keeps only this part")
                 .replace ("{FROM}", juce::String (p.inBeat / 4.0 + 1.0, 0)).replace ("{TO}", juce::String (p.outBeat / 4.0 + 1.0, 0)));
    playlist.repaint();
}

void MainComponent::savePunchSettings()
{
    settings()->setValue ("punchPreRoll", session.getRecorder().getPunch().preRollBars);
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

    auto* w = new juce::AlertWindow (TRANS ("Export audio"),
                                     TRANS ("Renders on this PC with your plugins and audio files. Changes made meanwhile (by anyone) don't get into it."),
                                     juce::MessageBoxIconType::NoIcon);

    const auto patternName = state.getPattern()[song::ids::name].toString();
    w->addComboBox ("what", { TRANS ("The whole song (playlist)"), TRANS ("Pattern \"{NAME}\" (as a seamless loop)").replace ("{NAME}", patternName) }, TRANS ("What"));

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

    w->addComboBox ("format", formats, TRANS ("Format"));
    w->addComboBox ("stems", { TRANS ("Just the mix"), TRANS ("The mix + one file per mixer insert (stems)") }, TRANS ("Files"));

    auto* prefs = settings();
    w->getComboBoxComponent ("what")->setSelectedItemIndex (session.isSongMode() ? 0 : 1, juce::dontSendNotification);
    w->getComboBoxComponent ("format")->setSelectedItemIndex (juce::jlimit (0, formats.size() - 1, prefs->getIntValue ("exportFormat", 0)),
                                                              juce::dontSendNotification);
    w->getComboBoxComponent ("stems")->setSelectedItemIndex (prefs->getBoolValue ("exportStems", false) ? 1 : 0, juce::dontSendNotification);

    w->addButton (TRANS ("Export..."), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

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

        fileChooser = std::make_unique<juce::FileChooser> (TRANS ("Export to"), exportsFolder().getChildFile (base + ext), "*" + ext);
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
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Export"), error);
        return;
    }

    appendLog (TRANS ("Exporting to {FILE}...").replace ("{FILE}", opts.destination.getFileName()));
    exportProgress = 0.0;

    auto* w = new juce::AlertWindow (TRANS ("Exporting..."), opts.destination.getFileName(), juce::MessageBoxIconType::NoIcon);
    w->addProgressBarComponent (exportProgress);
    w->addButton (TRANS ("Cancel"), 0);
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
        appendLog (TRANS ("Export cancelled"));
        return;
    }

    if (r.error.isNotEmpty())
    {
        appendLog (TRANS ("Export failed") + ": " + r.error);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Export failed"), r.error);
        return;
    }

    juce::String text;
    juce::int64 bytes = 0;

    for (auto& f : r.files)
        bytes += f.getSize();

    const auto main = r.files.isEmpty() ? juce::File() : r.files.getFirst();
    text << TRANS ("Saved") << " " << main.getFileName() << " (" << juce::File::descriptionOfSizeInBytes (main.getSize()) << ")";

    if (r.files.size() > 1)
        text << "\n+ " << TRANS ("{N} stems in {FOLDER}").replace ("{N}", juce::String (r.files.size() - 1))
                                                  .replace ("{FOLDER}", "\"" + main.getFileNameWithoutExtension() + " - stems\"");

    if (r.missingAudio > 0)
        text << "\n\n" << TRANS ("Note: {N} audio file(s) hadn't arrived on this PC yet, so they're silent in it.").replace ("{N}", juce::String (r.missingAudio));

    if (r.missingPlugins > 0)
        text << "\n\n" << TRANS ("Note: {N} plugin(s) aren't installed on this PC, so they're silent / bypassed in it.").replace ("{N}", juce::String (r.missingPlugins));

    appendLog (TRANS ("Exported") + " " + main.getFullPathName() + " (" + juce::File::descriptionOfSizeInBytes (bytes) + ")");

    auto* w = new juce::AlertWindow (TRANS ("Export finished"), text, juce::MessageBoxIconType::InfoIcon);
    w->addButton (TRANS ("Show in folder"), 1);
    w->addButton (TRANS ("OK"), 0, juce::KeyPress (juce::KeyPress::returnKey));
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
    auto& recorder = session.getRecorder();
    const auto disarmed = recorder.getDisarmedWithInput();
    auto* edit = state.getEdit();

    if (! disarmed.isEmpty() && edit != nullptr)
    {
        m.addSectionHeader (TRANS ("Nothing is armed. Arm:"));

        for (int i = 0; i < disarmed.size(); ++i)
        {
            auto* insert = song::findInsert (*edit, disarmed[i]);
            juce::String inputName;

            for (auto& in : inputs)
                if (in.id == recorder.getInputFor (disarmed[i]))
                    inputName = in.name;

            m.addItem (200 + i, (insert != nullptr ? insert->getName() : juce::String ("?")) + "  (" + inputName + ")");
        }

        if (disarmed.size() > 1)
            m.addItem (199, TRANS ("All of them"));

        m.addSeparator();
        m.addSectionHeader (TRANS ("Or record from a new insert:"));
    }
    else
    {
        m.addSectionHeader (TRANS ("Nothing is armed. Record from:"));
    }

    for (int i = 0; i < inputs.size(); ++i)
        m.addItem (100 + i, inputs[i].name);

    if (inputs.isEmpty())
        m.addItem (-1, TRANS ("(no inputs found)"), false);

    m.addSeparator();
    m.addItem (1, TRANS ("Audio settings..."));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordButton), [this, inputs, disarmed] (int r)
    {
        auto* edit = state.getEdit();

        if (r == 1)
            showAudioSettings();

        if (r == 199 || juce::isPositiveAndBelow (r - 200, disarmed.size()))
        {
            auto& rec = session.getRecorder();

            if (r == 199)
                rec.armAllWithInput();
            else
                rec.setArmed (disarmed[r - 200], true);

            appendLog (TRANS ("Armed: press Rec (or R) to record"));
            refresh();
            return;
        }

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
            appendLog (TRANS ("Insert \"{NAME}\" records from {INPUT} (you hear it through its effects). Add autotune/EQ to it if you like, then press Rec.")
                         .replace ("{NAME}", name).replace ("{INPUT}", inputs[r - 100].name));
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
        title << "  (" << TRANS ("the host's song") << ")";

    title << "  -  COTOSTUDIO";

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

    const auto templates = appdata::findTemplates();
    const auto startFrom = defaultTemplate();

    m.addItem (item (1, startFrom.existsAsFile() ? TRANS ("New song from \"{NAME}\"").replace ("{NAME}", startFrom.getFileNameWithoutExtension())
                                                 : TRANS ("New song"), canReplace, "Ctrl+N"));

    // Templates: songs to start from (e.g. your recording setup with all its plugins).
    juce::PopupMenu templateMenu, defaultMenu;

    if (templates.isEmpty())
        templateMenu.addItem (-1, TRANS ("(no templates yet: save one below)"), false);

    for (int i = 0; i < templates.size(); ++i)
        templateMenu.addItem (300 + i, TRANS ("New song from") + " \"" + templates[i].getFileNameWithoutExtension() + "\"", canReplace);

    templateMenu.addSeparator();
    templateMenu.addItem (30, TRANS ("Save this song as a template..."));

    defaultMenu.addItem (31, TRANS ("An empty song"), true, ! startFrom.existsAsFile());

    for (int i = 0; i < templates.size(); ++i)
        defaultMenu.addItem (400 + i, templates[i].getFileNameWithoutExtension(), true, templates[i] == startFrom);

    templateMenu.addSubMenu (TRANS ("New song (Ctrl+N) starts from"), defaultMenu);
    templateMenu.addItem (32, TRANS ("New empty song"), canReplace);
    templateMenu.addItem (33, TRANS ("Open the Templates folder"));
    m.addSubMenu (TRANS ("Templates"), templateMenu);

    m.addItem (item (2, TRANS ("Open..."), canReplace, "Ctrl+O"));

    recentFiles.createPopupMenuItems (recent, 100, false, true);

    if (recent.getNumItems() == 0)
        recent.addItem (-1, TRANS ("(none yet)"), false);

    m.addSubMenu (TRANS ("Open recent"), recent, canReplace);
    m.addSeparator();
    m.addItem (item (3, TRANS ("Save"), true, "Ctrl+S"));
    m.addItem (item (4, TRANS ("Save as..."), true, "Ctrl+Shift+S"));
    m.addSeparator();
    m.addItem (item (6, TRANS ("Export audio (WAV, FLAC, MP3)..."), ! exporter.isRunning(), "Ctrl+R"));
    m.addSeparator();
    m.addItem (5, TRANS ("Open an automatic backup..."), canReplace);
    m.addItem (7, currentFile != juce::File() ? TRANS ("Show this project's folder") : TRANS ("Show the Projects folder"));

    juce::PopupMenu data;
    data.addItem (8, TRANS ("Open") + " " + appdata::getRoot().getFullPathName());
    data.addItem (9, TRANS ("Use another data folder..."));
    m.addSubMenu (TRANS ("Data folder"), data);

    if (! canReplace)
    {
        m.addSeparator();
        m.addItem (-1, TRANS ("(only the host can open or start songs during a session)"), false);
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (fileButton), [this, templates] (int r)
    {
        if (juce::isPositiveAndBelow (r - 300, templates.size()))
        {
            newSongFromTemplate (templates[r - 300]);
            return;
        }

        if (juce::isPositiveAndBelow (r - 400, templates.size()) || r == 31)
        {
            const auto chosen = r == 31 ? juce::File() : templates[r - 400];
            settings()->setValue ("newSongTemplate", chosen.getFileNameWithoutExtension());
            appendLog (r == 31 ? TRANS ("New song (Ctrl+N) starts an empty song")
                               : TRANS ("New song (Ctrl+N) starts from the template \"{NAME}\"").replace ("{NAME}", chosen.getFileNameWithoutExtension()));
            return;
        }

        if (r == 30)       saveAsTemplate();
        else if (r == 32)  newEmptySong();
        else if (r == 33)  { appdata::templatesFolder().createDirectory(); appdata::templatesFolder().startAsProcess(); }
        else if (r == 1)   newSong();
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
                fileChooser = std::make_unique<juce::FileChooser> (TRANS ("Open an automatic backup"), autosaveFolder(), appdata::songWildcards);
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
    fileChooser = std::make_unique<juce::FileChooser> (TRANS ("Where COTOSTUDIO keeps everything (projects, backups, exports, settings)"),
                                                       appdata::getRoot().getParentDirectory());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();

        if (! folder.isDirectory() || folder == appdata::getRoot())
            return;

        appdata::chooseRoot (folder);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, TRANS ("Data folder"),
            TRANS ("From the next start COTOSTUDIO keeps everything in\n{NEW}\n\nYour current files stay in {OLD} - move the Projects folder over yourself if you want them there too.")
                .replace ("{NEW}", folder.getFullPathName()).replace ("{OLD}", appdata::getRoot().getFullPathName()));
    });
}

void MainComponent::askToSaveChanges (std::function<void()> proceed)
{
    if (! dirty || session.getMode() == collab::Session::Mode::joined)
    {
        proceed();   // (a joiner's copy is the host's song: nothing of theirs to lose)
        return;
    }

    auto options = juce::MessageBoxOptions::makeOptionsYesNoCancel (juce::MessageBoxIconType::QuestionIcon, TRANS ("Save changes?"),
                                                                    TRANS ("Save the changes to \"{NAME}\" first?").replace ("{NAME}", session.getSongTitle()),
                                                                    TRANS ("Save"), TRANS ("Don't save"), TRANS ("Cancel"), this);
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

juce::File MainComponent::defaultTemplate() const
{
    const auto name = userSettings->getValue ("newSongTemplate");
    return name.isNotEmpty() ? appdata::templateFileForName (name) : juce::File();
}

void MainComponent::newSong()
{
    // New song starts from your chosen template, if you picked one (File > Templates).
    const auto t = defaultTemplate();

    if (t.existsAsFile())
        newSongFromTemplate (t);
    else
        newEmptySong();
}

void MainComponent::newSongFromTemplate (const juce::File& file)
{
    if (! session.canReplaceSong())
    {
        appendLog (TRANS ("Only the host can start a new song during a session"));
        return;
    }

    askToSaveChanges ([this, file]
    {
        if (session.isBusyWithFile())
        {
            appendLog (TRANS ("Still busy saving or opening the last song..."));
            return;
        }

        const auto name = file.getFileNameWithoutExtension();
        appendLog (TRANS ("New song from the template \"{NAME}\"...").replace ("{NAME}", name));
        expectingOwnSong = true;

        session.openProject (file, [safe = juce::Component::SafePointer<MainComponent> (this), file, name] (juce::String error)
        {
            if (safe == nullptr)
                return;

            auto& self = *safe;
            self.expectingOwnSong = false;

            if (error.isNotEmpty())
            {
                self.appendLog (TRANS ("Could not open {FILE}").replace ("{FILE}", file.getFileName()) + ": " + error);
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Could not open the template"), error);
                return;
            }

            // A new, untitled song: Save asks for its name (the template itself stays as it is).
            self.currentFile = juce::File();
            self.savedHash = self.lastAutosaveHash = self.currentHash();
            self.dirty = false;
            self.state.viewedPattern = self.state.selectedChannel = self.state.placeAudioChannel = self.state.selectedInsert = {};

            // The template's recording inputs (which input feeds which insert, armed or not).
            const auto inputs = file.getSiblingFile ("Inputs.txt");

            if (inputs.existsAsFile())
                self.session.getRecorder().restoreChoices (inputs.loadFileAsString());

            const int armed = self.session.getRecorder().getNumArmed();
            self.appendLog (TRANS ("New song from \"{NAME}\"").replace ("{NAME}", name)
                             + (armed > 0 ? " - " + TRANS ("{N} insert(s) armed: press Rec to record").replace ("{N}", juce::String (armed)) : juce::String()));
            self.refresh();
        }, "Untitled");
    });
}

void MainComponent::saveAsTemplate()
{
    if (session.isBusyWithFile())
    {
        appendLog (TRANS ("Still busy saving or opening the last song..."));
        return;
    }

    auto* w = new juce::AlertWindow (TRANS ("Save as a template"),
                                     TRANS ("A template is a song to start new ones from: channels, mixer inserts and their effects (with their settings), patterns, tempo and the sounds in it, plus which input records into which insert on this PC.\n\nName of the template:"),
                                     juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", session.getSongTitle() == "Untitled" ? TRANS ("Recording") : session.getSongTitle());
    w->addButton (TRANS ("Save"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<MainComponent> (this), w] (int r)
    {
        const auto name = w->getTextEditorContents ("name").trim();

        if (safe == nullptr || r != 1 || name.isEmpty())
            return;

        const auto file = appdata::templateFileForName (name);

        auto write = [safe, file]
        {
            if (safe == nullptr)
                return;

            const auto inputs = safe->session.getRecorder().saveChoices (true);
            safe->appendLog (TRANS ("Saving the template \"{NAME}\"...").replace ("{NAME}", file.getFileNameWithoutExtension()));

            safe->session.saveProject (file, true, [safe, file, inputs] (project::SaveResult result)
            {
                if (safe == nullptr)
                    return;

                if (! result.ok())
                {
                    safe->appendLog (TRANS ("Could not save") + ": " + result.error);
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Could not save the template"), result.error);
                    return;
                }

                file.getSiblingFile ("Inputs.txt").replaceWithText (inputs);
                safe->appendLog (TRANS ("Template \"{NAME}\" saved. File > Templates starts a new song from it (or make it what New song starts from)")
                                   .replace ("{NAME}", file.getFileNameWithoutExtension()));
            });
        };

        if (file.existsAsFile())
        {
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, TRANS ("Replace it?"),
                                                                         TRANS ("There is already a template called \"{NAME}\". Replace it with this song?")
                                                                           .replace ("{NAME}", file.getFileNameWithoutExtension()),
                                                                         TRANS ("Replace"), TRANS ("Cancel"), safe.getComponent());
            juce::AlertWindow::showAsync (options, [write] (int result) { if (result == 1) write(); });
            return;
        }

        write();
    }), true);
}

void MainComponent::newEmptySong()
{
    if (! session.canReplaceSong())
    {
        appendLog (TRANS ("Only the host can start a new song during a session"));
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
        appendLog (TRANS ("New song"));
        refresh();
    });
}

void MainComponent::openSong()
{
    if (! session.canReplaceSong())
    {
        appendLog (TRANS ("Only the host can open a song during a session (you can still save your copy)"));
        return;
    }

    askToSaveChanges ([this]
    {
        songsFolder().createDirectory();
        fileChooser = std::make_unique<juce::FileChooser> (TRANS ("Open a song"), currentFile.existsAsFile() ? currentFile : songsFolder(),
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
        appendLog (TRANS ("Still busy saving or opening the last song..."));
        return;
    }

    appendLog (TRANS ("Opening {FILE}...").replace ("{FILE}", file.getFileName()));
    expectingOwnSong = true;

    session.openProject (file, [safe = juce::Component::SafePointer<MainComponent> (this), file] (juce::String error)
    {
        if (safe == nullptr)
            return;

        auto& self = *safe;
        self.expectingOwnSong = false;

        if (error.isNotEmpty())
        {
            self.appendLog (TRANS ("Could not open {FILE}").replace ("{FILE}", file.getFileName()) + ": " + error);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Could not open the song"), error);
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
        appendLog (TRANS ("Still busy saving or opening the last song..."));

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
    auto* w = new juce::AlertWindow (TRANS ("Save the song"), TRANS ("Name of the project (it gets its own folder in {FOLDER})")
                                                                .replace ("{FOLDER}", songsFolder().getFullPathName()), juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", session.getSongTitle());
    w->addButton (TRANS ("Save"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

    appendLog (TRANS ("Saving \"{NAME}\"...").replace ("{NAME}", title));

    session.saveProject (file, true, [safe = juce::Component::SafePointer<MainComponent> (this), file, title, then]
                                     (project::SaveResult result)
    {
        if (safe == nullptr)
            return;

        auto& self = *safe;

        if (! result.ok())
        {
            self.appendLog (TRANS ("Could not save") + ": " + result.error);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, TRANS ("Could not save the song"), result.error);

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

        self.appendLog (TRANS ("Saved \"{NAME}\" in {FOLDER}").replace ("{NAME}", title).replace ("{FOLDER}", file.getParentDirectory().getFullPathName())
                         + (result.audioIncluded > 0 ? " (" + TRANS ("{N} audio files in Audio / Takes").replace ("{N}", juce::String (result.audioIncluded))
                                                         + (result.audioWritten > 0 ? ", " + TRANS ("{N} new").replace ("{N}", juce::String (result.audioWritten)) + ")" : juce::String (")"))
                                                     : juce::String()));

        if (result.audioMissing > 0)
            self.appendLog (TRANS ("{N} audio file(s) hadn't arrived on this PC yet and aren't in it").replace ("{N}", juce::String (result.audioMissing)));

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
            safe->appendLog (TRANS ("Automatic backup failed") + ": " + result.error);
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
    o.dialogTitle = TRANS ("Plugins - use Options > Scan for new or updated VST3 plug-ins");
    o.dialogBackgroundColour = theme::colours::panel;
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
    o.dialogTitle = TRANS ("Audio settings");
    o.dialogBackgroundColour = theme::colours::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.launchAsync();
}

void MainComponent::showSessionPanel()
{
    SessionPanel::Callbacks callbacks;
    callbacks.host = [this] (int port) { session.startHosting (port); refresh(); };
    callbacks.join = [this] (const juce::String& address, int port) { session.join (address, port); refresh(); };
    callbacks.leave = [this] { session.leave(); refresh(); };

    auto panel = std::make_unique<SessionPanel> (session, *settings(), std::move (callbacks));
    juce::CallOutBox::launchAsynchronously (std::move (panel), sessionPill.getScreenBounds(), nullptr);
}

void MainComponent::showSettingsMenu()
{
    juce::PopupMenu m, language;
    const auto current = settings()->getValue ("language", "en");

    m.addSectionHeader (TRANS ("Settings"));
    m.addItem (1, TRANS ("Sound card and MIDI..."));
    m.addItem (2, TRANS ("Find my plugins (VST3)..."));
    m.addSeparator();

    language.addItem (10, juce::String ("English"), true, current != "es");
    language.addItem (11, juce::String::fromUTF8 ("Espa\xc3\xb1ol"), true, current == "es");
    m.addSubMenu (TRANS ("Language"), language);

    juce::PopupMenu data;
    data.addItem (20, TRANS ("Open") + " " + appdata::getRoot().getFullPathName());
    data.addItem (21, TRANS ("Use another data folder..."));
    m.addSubMenu (TRANS ("Data folder"), data);

    m.addSeparator();
    m.addItem (3, TRANS ("Show every message"));
    m.addItem (4, TRANS ("About COTOSTUDIO"));

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (settingsButton), [this, current] (int r)
    {
        if (r == 1)        showAudioSettings();
        else if (r == 2)   showPluginScanner();
        else if (r == 3)   showLog();
        else if (r == 4)   showAbout();
        else if (r == 20)  appdata::getRoot().startAsProcess();
        else if (r == 21)  chooseDataFolder();
        else if (r == 10 || r == 11)
        {
            const juce::String code = r == 11 ? "es" : "en";

            if (code == current)
                return;

            settings()->setValue ("language", code);
            settings()->saveIfNeeded();

            // Both languages, so whoever reads it understands.
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                code == "es" ? juce::String::fromUTF8 ("Espa\xc3\xb1ol") : juce::String ("English"),
                code == "es" ? juce::String::fromUTF8 ("COTOSTUDIO estar\xc3\xa1 en espa\xc3\xb1ol la pr\xc3\xb3xima vez que lo abras.\n"
                                                       "(COTOSTUDIO will be in Spanish the next time you open it.)")
                             : juce::String::fromUTF8 ("COTOSTUDIO will be in English the next time you open it.\n"
                                                       "(COTOSTUDIO estar\xc3\xa1 en ingl\xc3\xa9s la pr\xc3\xb3xima vez que lo abras.)"));
        }
    });
}

void MainComponent::showAbout()
{
    struct About  : public juce::Component
    {
        About()
        {
            image = theme::logoFull();
            setSize (420, 330);
        }

        void paint (juce::Graphics& g) override
        {
            using namespace theme::colours;
            g.fillAll (panel);
            auto r = getLocalBounds().reduced (20);

            if (image.isValid())
                g.drawImage (image, r.removeFromTop (150).toFloat(), juce::RectanglePlacement::centred);

            r.removeFromTop (12);
            g.setColour (text);
            g.setFont (theme::ui (18.0f, theme::Weight::bold));
            g.drawText ("COTOSTUDIO " + juce::JUCEApplication::getInstance()->getApplicationVersion(),
                        r.removeFromTop (26), juce::Justification::centred, false);

            g.setColour (muted);
            g.setFont (theme::ui (12.5f));
            g.drawFittedText (TRANS ("Make music together, in sync, over Tailscale.") + "\n"
                              + TRANS ("Free software (GNU AGPLv3). Built on Tracktion Engine and JUCE.") + "\n"
                              + TRANS ("Fonts: Inter and JetBrains Mono (SIL Open Font License).") + "\n"
                              + "ASIO is a trademark and software of Steinberg Media Technologies GmbH.",
                              r, juce::Justification::centredTop, 6);
        }

        juce::Image image;
    };

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new About());
    o.dialogTitle = TRANS ("About COTOSTUDIO");
    o.dialogBackgroundColour = theme::colours::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void MainComponent::showLog()
{
    if (logWindow == nullptr)
    {
        struct LogWindow  : public juce::DocumentWindow
        {
            LogWindow() : DocumentWindow (TRANS ("Messages") + "  -  COTOSTUDIO", theme::colours::panel, DocumentWindow::closeButton) {}
            void closeButtonPressed() override   { setVisible (false); }
        };

        logWindow = std::make_unique<LogWindow>();
        logWindow->setUsingNativeTitleBar (true);
        logWindow->setResizable (true, false);
        logWindow->setContentNonOwned (&logView, false);
        logWindow->centreAroundComponent (this, 760, 420);
    }

    logView.moveCaretToEnd();
    logWindow->setVisible (true);
    logWindow->toFront (true);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::colours::bg);

    // The top bar
    g.setColour (theme::colours::panel);
    g.fillRect (getLocalBounds().removeFromTop (topBarHeight));
    g.setColour (theme::colours::line);
    g.fillRect (0, topBarHeight - 1, getWidth(), 1);

    // Separators between groups in the top bar
    g.setColour (theme::colours::line);

    for (auto x : topBarSeparators)
        g.fillRect (x, 14, 1, topBarHeight - 28);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    statusBar.setBounds (r.removeFromBottom (StatusBar::height));

    // Top bar. Narrow windows drop words first, then whole items, so it always fits:
    // level 0 is everything; each level up is more compact.
    auto fullBar = r.removeFromTop (topBarHeight).reduced (10, 0);
    const int h = 30;

    struct Widths
    {
        int logo, sounds, song, click, lcd, pattern, length, session, focus, share;
        bool undoRedo, separators;
    };

    auto widthsFor = [] (int level) -> Widths
    {
        switch (level)
        {
            case 0:  return { 112, 98, 72, 80, LcdDisplay::preferredWidth, 150, 84, SessionPill::preferredWidth, 84, 122, true, true };
            case 1:  return { 112, 36, 62, 66, LcdDisplay::preferredWidth, 124, 84, 196, 36, 108, true, true };
            case 2:  return { 0,   36, 62, 66, LcdDisplay::preferredWidth, 116, 80, 150, 36, 104, true, true };
            case 3:  return { 0,   36, 62, 36, LcdDisplay::compactWidth,   110, 74, 150, 36, 100, true, true };
            default: return { 0,   36, 62, 36, LcdDisplay::compactWidth,   104, 70, 64,  36, 96,  false, false };
        }
    };

    auto needed = [] (const Widths& x)
    {
        return (x.logo > 0 ? x.logo + 10 : 0) + 74 + 6 + x.sounds + 6 + (x.separators ? 10 : 0)
               + 50 + x.song + 10 + 48 + 4 + 36 + 4 + x.click + 10 + x.lcd + 10
               + x.pattern + 4 + 30 + 4 + 30 + 4 + x.length + 8
               + 32 + 4 + (x.undoRedo ? 32 + 4 + 32 + 10 : 0) + (x.separators ? 10 : 0)
               + x.session + 10 + x.focus + 6 + x.share + 6;
    };

    int level = 0;

    while (level < 4 && needed (widthsFor (level)) > fullBar.getWidth())
        ++level;

    const auto x = widthsFor (level);
    auto bar = fullBar;
    topBarSeparators.clearQuick();

    auto place = [&bar, h] (juce::Component& c, int width, int gapAfter = 6)
    {
        c.setBounds (bar.removeFromLeft (width).withSizeKeepingCentre (width, h));
        bar.removeFromLeft (gapAfter);
    };

    auto placeRight = [&bar, h] (juce::Component& c, int width, int gapBefore = 6)
    {
        c.setBounds (bar.removeFromRight (width).withSizeKeepingCentre (width, h));
        bar.removeFromRight (gapBefore);
    };

    auto separator = [this, &bar, &x] (bool left)
    {
        if (! x.separators)
            return;

        topBarSeparators.add (left ? bar.getX() + 4 : bar.getRight() - 5);

        if (left) bar.removeFromLeft (10);
        else      bar.removeFromRight (10);
    };

    logo->setVisible (x.logo > 0);

    if (x.logo > 0)
    {
        logo->setBounds (bar.removeFromLeft (x.logo).withSizeKeepingCentre (x.logo, 46));
        bar.removeFromLeft (10);
    }

    place (fileButton, 74);
    soundsButton.setButtonText (x.sounds < 60 ? juce::String() : TRANS ("Sounds"));
    place (soundsButton, x.sounds);
    soundsButton.setToggleState (soundsVisible, juce::dontSendNotification);
    separator (true);

    place (patModeButton, 50, 0);
    place (songModeButton, x.song, 10);
    place (playButton, 48, 4);
    place (recordButton, 36, 4);
    clickButton.getProperties().set ("iconOnly", x.click < 60);
    place (clickButton, x.click, 10);

    lcd.setCompact (x.lcd < LcdDisplay::preferredWidth);
    lcd.setBounds (bar.removeFromLeft (x.lcd).withSizeKeepingCentre (x.lcd, 44));
    tempoSlider.setBounds (lcd.getTempoArea() + lcd.getPosition());
    bar.removeFromLeft (10);

    place (patternBox, x.pattern, 4);
    place (newPatternButton, 30, 4);
    place (patternMenuButton, 30, 4);
    place (lengthBox, x.length, 8);

    // From the right: settings, undo/redo, session, focus, share.
    placeRight (settingsButton, 32, 4);
    undoButton.setVisible (x.undoRedo);
    redoButton.setVisible (x.undoRedo);

    if (x.undoRedo)
    {
        placeRight (redoButton, 32, 4);
        placeRight (undoButton, 32, 10);
    }

    separator (false);
    sessionPill.setBounds (bar.removeFromRight (x.session).withSizeKeepingCentre (x.session, 38));
    bar.removeFromRight (10);
    focusButton.setButtonText (x.focus < 60 ? juce::String() : TRANS ("Focus"));
    placeRight (focusButton, x.focus, 6);
    placeRight (shareButton, x.share, 6);

    // Body
    r.reduce (8, 8);

    sounds.setVisible (soundsVisible);

    if (soundsVisible)
    {
        sounds.setBounds (r.removeFromLeft (SoundBrowser::defaultWidth));
        r.removeFromLeft (8);
    }

    browser.setVisible (browserVisible);
    showBrowserButton.setVisible (! browserVisible);

    if (browserVisible)
        browser.setBounds (r.removeFromLeft (Browser::defaultWidth));
    else
        showBrowserButton.setBounds (r.removeFromLeft (20));

    r.removeFromLeft (8);

    // Playlist, rack and the piano roll / mixer / automation tabs - or one of them
    // filling the space (full screen), or the rack's note while it has its own window.
    const bool rackHere = ! isDetached (Panel::rack);
    auto* rackStrip = placeholders[(int) Panel::rack].get();
    const bool normal = maximised == Maximised::none;

    playlist.setVisible (maximised != Maximised::pianoRoll);
    dock->setVisible (maximised != Maximised::playlist);
    divider1.setVisible (normal && rackHere);
    divider2.setVisible (normal && rackHere);
    dividerNoRack.setVisible (normal && ! rackHere);

    if (rackHere)
        rack.setVisible (normal);

    if (rackStrip != nullptr)
    {
        if (! rackHere && normal)
            addAndMakeVisible (rackStrip);
        else
            rackStrip->setVisible (false);
    }

    if (maximised == Maximised::playlist)
    {
        playlist.setBounds (r);
    }
    else if (maximised == Maximised::pianoRoll)
    {
        dock->setBounds (r);
    }
    else if (rackHere)
    {
        juce::Component* panels[] = { &playlist, &divider1, &rack, &divider2, dock.get() };
        panelLayout.layOutComponents (panels, 5, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
    }
    else
    {
        juce::Component* panels[] = { &playlist, &dividerNoRack, rackStrip, dock.get() };
        panelLayoutNoRack.layOutComponents (panels, 4, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
    }

    bottomTabs.setBounds (dock->getLocalBounds().reduced (1));
}

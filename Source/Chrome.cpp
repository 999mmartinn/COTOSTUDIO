#include "Chrome.h"

using namespace theme::colours;

//==============================================================================
LcdDisplay::LcdDisplay (EditorState& s) : state (s)
{
    setOpaque (false);
    setTooltip (TRANS ("Song position. Right-click: show bars and beats or minutes and seconds"));
    startTimerHz (25);
}

LcdDisplay::~LcdDisplay()   { stopTimer(); }

juce::Rectangle<int> LcdDisplay::getTempoArea() const
{
    return getLocalBounds().withTrimmedLeft (positionWidth()).reduced (6, 0).withTrimmedTop (15).withTrimmedBottom (3);
}

juce::String LcdDisplay::positionText (juce::String& timeText) const
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
    {
        timeText = "-:--.---";
        return "---.-.-";
    }

    const bool playing = edit->getTransport().isPlaying();
    const double beat = playing ? juce::jmax (0.0, edit->tempoSequence.toBeats (edit->getTransport().getPosition()).inBeats())
                                : state.getPlayStartBeat();
    const double seconds = playing ? edit->getTransport().getPosition().inSeconds() : state.beatsToSeconds (beat);

    const int bar = (int) std::floor (beat / 4.0 + 1.0e-9) + 1;
    const int beatInBar = (int) std::floor (std::fmod (beat, 4.0) + 1.0e-9) + 1;
    const int step = (int) std::floor (std::fmod (beat, 1.0) * 4.0 + 1.0e-9) + 1;

    const int ms = (int) std::floor (juce::jmax (0.0, seconds) * 1000.0);
    timeText = juce::String (ms / 60000) + ":" + juce::String ((ms / 1000) % 60).paddedLeft ('0', 2) + "."
                + juce::String (ms % 1000).paddedLeft ('0', 3);

    return juce::String (bar).paddedLeft ('0', 3) + "." + juce::String (beatInBar) + "." + juce::String (step);
}

void LcdDisplay::timerCallback()
{
    juce::String time;
    const auto text = positionText (time) + time;

    if (text != lastText)
    {
        lastText = text;
        repaint (getLocalBounds().withWidth (positionWidth()));
    }
}

void LcdDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (lcd);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r, 8.0f, 1.0f);

    juce::String time;
    const auto pos = positionText (time);
    auto area = getLocalBounds().reduced (12, 3);

    auto label = [&g] (juce::Rectangle<int> a, const juce::String& t)
    {
        g.setColour (lcdMuted);
        g.setFont (theme::ui (9.5f, theme::Weight::bold));
        g.drawText (t, a.removeFromTop (12), juce::Justification::bottomLeft, false);
    };

    // The big number is bars (default) or minutes:seconds; the small box shows the other.
    const auto bigText   = timeFirst ? time : pos;
    const auto smallText = timeFirst ? pos : time;
    const auto bigLabel   = timeFirst ? TRANS ("TIME") : TRANS ("BAR");
    const auto smallLabel = timeFirst ? TRANS ("BAR") : TRANS ("TIME");

    auto posArea = area.removeFromLeft (104);
    label (posArea, bigLabel);
    g.setColour (lcdText);
    g.setFont (theme::mono (19.0f, true));
    g.drawFittedText (bigText, posArea.withTrimmedTop (12), juce::Justification::centredLeft, 1, 0.8f);

    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRect (area.removeFromLeft (1).withTrimmedTop (4).withTrimmedBottom (4));

    if (! compact)
    {
        area.removeFromLeft (10);
        auto timeArea = area.removeFromLeft (86);
        label (timeArea, smallLabel);
        g.setColour (lcdText);
        g.setFont (theme::mono (15.0f));
        g.drawFittedText (smallText, timeArea.withTrimmedTop (12), juce::Justification::centredLeft, 1, 0.8f);

        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (area.removeFromLeft (1).withTrimmedTop (4).withTrimmedBottom (4));
    }

    area.removeFromLeft (8);
    label (area, "BPM");
}

void LcdDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;

    juce::PopupMenu m;
    m.addSectionHeader (TRANS ("Counter shows"));
    m.addItem (1, TRANS ("Bars and beats (bar.beat.step)"), true, ! timeFirst);
    m.addItem (2, TRANS ("Minutes and seconds (min:sec.ms)"), true, timeFirst);

    juce::Component::SafePointer<LcdDisplay> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        safe->setTimeFirst (result == 2);

        if (safe->onTimeFirstChanged)
            safe->onTimeFirstChanged (safe->timeFirst);
    });
}

//==============================================================================
SessionPill::SessionPill (collab::Session& s) : juce::Button ("session"), session (s)
{
    setTooltip (TRANS ("Your session: host one or join a friend's (click)"));
}

void SessionPill::paintButton (juce::Graphics& g, bool over, bool down)
{
    using Mode = collab::Session::Mode;
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (down ? well.darker (0.1f) : (over ? raised : well));
    g.fillRoundedRectangle (r, 9.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r, 9.0f, 1.0f);

    auto area = getLocalBounds().reduced (6, 4);
    const auto people = session.getPeople();
    const auto mode = session.getMode();

    // Coloured initials, overlapping a little.
    auto avatars = area.removeFromLeft (juce::jmin (4, juce::jmax (1, people.size())) * 17 + 7).toFloat();
    float x = avatars.getX();

    for (int i = 0; i < juce::jmin (4, people.size()); ++i)
    {
        const auto circle = juce::Rectangle<float> (x, avatars.getCentreY() - 12.0f, 24.0f, 24.0f);
        g.setColour (well);
        g.fillEllipse (circle);
        g.setColour (people[i].colour.isTransparent() ? accent : people[i].colour);
        g.fillEllipse (circle.reduced (2.0f));
        g.setColour (juce::Colours::white);
        g.setFont (theme::ui (11.0f, theme::Weight::bold));
        g.drawText (people[i].name.substring (0, 1).toUpperCase(), circle, juce::Justification::centred, false);
        x += 17.0f;
    }

    area.removeFromLeft (6);
    auto top = area.removeFromTop (area.getHeight() / 2);

    // Status: green dot = connected and in sync, amber = checking, grey = offline.
    const auto dot = mode == Mode::offline ? dim : (mode == Mode::connecting || ! session.isInSync()) ? amber : green;
    g.setColour (dot);
    g.fillEllipse (top.removeFromLeft (8).toFloat().withSizeKeepingCentre (7.0f, 7.0f).translated (0.0f, 1.0f));
    top.removeFromLeft (5);

    juce::String status, detail;

    switch (mode)
    {
        case Mode::offline:     status = TRANS ("Offline");  detail = TRANS ("Click to host or join"); break;
        case Mode::connecting:  status = TRANS ("Connecting..."); break;
        case Mode::hosting:
        {
            status = TRANS ("Hosting") + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (people.size()) + " "
                      + (people.size() == 1 ? TRANS ("person") : TRANS ("people"));
            const auto ts = collab::Session::findTailscaleAddress();
            detail = ts.isNotEmpty() ? ts : TRANS ("waiting for friends");
            break;
        }
        case Mode::joined:
            status = TRANS ("Joined") + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (people.size()) + " " + TRANS ("people");
            detail = session.isInSync() ? TRANS ("in sync") : TRANS ("checking sync...");
            break;
    }

    g.setColour (theme::colours::text);
    g.setFont (theme::ui (11.5f, theme::Weight::bold));
    g.drawText (status, top, juce::Justification::bottomLeft, true);
    g.setColour (muted);
    g.setFont (mode == Mode::hosting ? theme::mono (10.5f) : theme::ui (10.5f));
    g.drawText (detail, area, juce::Justification::topLeft, true);
}

//==============================================================================
SessionPanel::SessionPanel (collab::Session& s, juce::PropertiesFile& p, Callbacks c)
    : session (s), settings (p), callbacks (std::move (c))
{
    using Mode = collab::Session::Mode;
    const bool offline = session.getMode() == Mode::offline;

    portEditor.setText (settings.getValue ("port", "47800"), false);
    portEditor.setInputRestrictions (5, "0123456789");
    portEditor.setJustification (juce::Justification::centred);
    addressEditor.setText (settings.getValue ("address", "127.0.0.1"), false);
    addressEditor.setTextToShowWhenEmpty ("100.x.y.z", dim);

    theme::setStyle (hostButton, "accent");
    theme::setIcon (hostButton, theme::Icon::share);
    theme::setIcon (joinButton, theme::Icon::link);

    auto close = [this]
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    };

    hostButton.onClick = [this, close]
    {
        settings.setValue ("port", portEditor.getText());

        if (callbacks.host)
            callbacks.host (portEditor.getText().getIntValue());

        close();
    };

    joinButton.onClick = [this, close]
    {
        settings.setValue ("port", portEditor.getText());
        settings.setValue ("address", addressEditor.getText().trim());

        if (callbacks.join)
            callbacks.join (addressEditor.getText().trim(), portEditor.getText().getIntValue());

        close();
    };

    leaveButton.onClick = [this, close]
    {
        if (callbacks.leave)
            callbacks.leave();

        close();
    };

    invite = collab::Session::findTailscaleAddress();
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (invite); };
    copyButton.setTooltip (TRANS ("Copy your address to send it to a friend"));

    for (auto* c : std::initializer_list<juce::Component*> { &portEditor, &addressEditor, &hostButton, &joinButton, &leaveButton, &copyButton })
        addAndMakeVisible (c);

    hostButton.setEnabled (offline);
    joinButton.setEnabled (offline);
    portEditor.setEnabled (offline);
    addressEditor.setEnabled (offline);
    leaveButton.setVisible (! offline);
    copyButton.setVisible (invite.isNotEmpty());
    setSize (330, offline ? 268 : 300);
}

void SessionPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().reduced (16, 12);
    g.setColour (text);
    g.setFont (theme::ui (15.0f, theme::Weight::bold));
    g.drawText (TRANS ("Session"), r.removeFromTop (22), juce::Justification::centredLeft, false);

    g.setColour (muted);
    g.setFont (theme::ui (12.0f));
    g.drawFittedText (session.getStatusText(), r.removeFromTop (34), juce::Justification::topLeft, 2);

    auto heading = [&g] (juce::Rectangle<int> a, const juce::String& t)
    {
        g.setColour (muted);
        g.setFont (theme::ui (10.5f, theme::Weight::bold));
        g.drawText (t.toUpperCase(), a, juce::Justification::bottomLeft, false);
    };

    heading (juce::Rectangle<int> (16, 76, 200, 16), TRANS ("Host - your friends join you"));
    heading (juce::Rectangle<int> (16, 150, 200, 16), TRANS ("Join a friend"));

    g.setColour (muted);
    g.setFont (theme::ui (12.0f));
    g.drawText (TRANS ("Port"), 16, 98, 40, 28, juce::Justification::centredLeft);

    if (invite.isNotEmpty())
    {
        g.setColour (text);
        g.setFont (theme::ui (12.0f));
        g.drawText (TRANS ("They type:"), 16, 128, 70, 18, juce::Justification::centredLeft);
        g.setColour (accent);
        g.setFont (theme::mono (12.5f, true));
        g.drawText (invite, 86, 128, 140, 18, juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (dim);
        g.setFont (theme::ui (11.5f));
        g.drawText (TRANS ("(over the internet: Tailscale, see the README)"), 16, 128, 300, 18, juce::Justification::centredLeft);
    }
}

void SessionPanel::resized()
{
    portEditor.setBounds (56, 98, 70, 28);
    hostButton.setBounds (getWidth() - 16 - 120, 98, 120, 28);
    copyButton.setBounds (getWidth() - 16 - 60, 126, 60, 22);
    addressEditor.setBounds (16, 172, getWidth() - 32 - 96, 30);
    joinButton.setBounds (getWidth() - 16 - 88, 172, 88, 30);
    leaveButton.setBounds (16, getHeight() - 16 - 30, getWidth() - 32, 30);
}

//==============================================================================
StatusBar::StatusBar (te::Engine& e, collab::Session& s) : engine (e), session (s)
{
    setOpaque (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip (TRANS ("Click to see every message"));
    timerCallback();
    startTimerHz (2);
}

StatusBar::~StatusBar()   { stopTimer(); }

void StatusBar::setMessage (const juce::String& m)
{
    message = m;
    repaint (messageArea);
}

void StatusBar::setPlaying (const juce::String& p)
{
    if (p != playing)
    {
        playing = p;
        repaint();
    }
}

void StatusBar::timerCallback()
{
    const auto cpu = TRANS ("CPU") + " " + juce::String (juce::roundToInt (engine.getDeviceManager().getCpuUsage() * 100.0)) + "%";
    juce::String device = TRANS ("no sound card");

    if (auto* d = engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
        device = d->getTypeName() + juce::String::fromUTF8 (" \xc2\xb7 ") + d->getName().substring (0, 28)
                  + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (d->getCurrentSampleRate() / 1000.0, 1) + " kHz"
                  + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (d->getCurrentBufferSizeSamples());

    if (cpu != cpuText || device != deviceText)
    {
        cpuText = cpu;
        deviceText = device;
        repaint();
    }
}

void StatusBar::paint (juce::Graphics& g)
{
    using Mode = collab::Session::Mode;
    g.fillAll (panel);
    g.setColour (line);
    g.fillRect (0, 0, getWidth(), 1);

    auto r = getLocalBounds().reduced (12, 0).withTrimmedTop (1);
    g.setFont (theme::ui (11.5f));

    // Right side, from the right.
    auto item = [&r, &g] (const juce::String& t, juce::Colour c, juce::Font f)
    {
        const int w = (int) juce::GlyphArrangement::getStringWidth (f, t) + 2;
        g.setColour (c);
        g.setFont (f);
        g.drawText (t, r.removeFromRight (w), juce::Justification::centredRight, false);
        r.removeFromRight (16);
    };

    item ("COTOSTUDIO " + juce::JUCEApplicationBase::getInstance()->getApplicationVersion(), text, theme::ui (11.5f, theme::Weight::bold));
    item (deviceText, muted, theme::ui (11.5f));
    item (cpuText, muted, theme::ui (11.5f));

    const auto mode = session.getMode();
    const bool synced = mode != Mode::offline && session.isInSync();

    if (mode != Mode::offline)
    {
        const auto t = synced ? TRANS ("In sync") : TRANS ("Checking sync...");
        const auto f = theme::ui (11.5f, theme::Weight::semibold);
        const int w = (int) juce::GlyphArrangement::getStringWidth (f, t) + 16;
        auto a = r.removeFromRight (w);
        g.setColour (synced ? green : amber);
        theme::drawIcon (g, synced ? theme::Icon::check : theme::Icon::wave, a.removeFromLeft (14).toFloat().withSizeKeepingCentre (12.0f, 12.0f),
                         synced ? green : amber, 2.0f);
        g.setFont (f);
        g.drawText (t, a, juce::Justification::centredRight, false);
        r.removeFromRight (16);
    }

    if (playing.isNotEmpty())
        item (playing, accent, theme::ui (11.5f, theme::Weight::semibold));

    messageArea = r;
    g.setColour (text.withAlpha (0.85f));
    g.setFont (theme::ui (11.5f));
    g.drawText (message, r, juce::Justification::centredLeft, true);
}

void StatusBar::mouseUp (const juce::MouseEvent& e)
{
    if (messageArea.contains (e.getPosition()) && onShowLog)
        onShowLog();
}

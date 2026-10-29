#pragma once

#include "EditorState.h"
#include "Theme.h"

/**
    The parts around the editors: the counter in the top bar, the session badge
    and its Host/Join panel, and the status bar at the bottom.
*/

//==============================================================================
/** The Pro Tools-style counter: bar.beat.step and time, with the tempo box on the right
    (the main window puts its tempo control in getTempoArea()). Repaints only while playing. */
class LcdDisplay  : public juce::Component,
                    public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    explicit LcdDisplay (EditorState&);
    ~LcdDisplay() override;

    juce::Rectangle<int> getTempoArea() const;
    /** A narrow column at the right end, for the time signature and tap tempo buttons. */
    juce::Rectangle<int> getSideArea() const;
    static constexpr int sideWidth = 40;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh()                          { repaint(); }

    /** True: the big number is minutes:seconds (bars go in the small box).
        Right-click the counter to switch; the main window saves the choice. */
    void setTimeFirst (bool t)              { if (t != timeFirst) { timeFirst = t; repaint(); } }
    bool isTimeFirst() const                { return timeFirst; }
    std::function<void (bool timeFirst)> onTimeFirstChanged;

    /** Narrow windows: bar and tempo only (no time). */
    void setCompact (bool c)                { if (c != compact) { compact = c; repaint(); } }

    static constexpr int preferredWidth = 330 + sideWidth, compactWidth = 250 + sideWidth;

private:
    EditorState& state;
    bool compact = false;
    bool timeFirst = false;
    int positionWidth() const               { return compact ? 125 : 214; }
    juce::String lastText;
    void timerCallback() override;
    juce::String positionText (juce::String& timeText) const;
};

//==============================================================================
/** Who's in the session (coloured initials) and its state; click for Host / Join / Leave. */
class SessionPill  : public juce::Button
{
public:
    explicit SessionPill (collab::Session&);
    void paintButton (juce::Graphics&, bool over, bool down) override;
    static constexpr int preferredWidth = 230;

private:
    collab::Session& session;
};

/** The panel that opens from the session badge. */
class SessionPanel  : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void (int port)> host;
        std::function<void (const juce::String& address, int port)> join;
        std::function<void()> leave;
    };

    SessionPanel (collab::Session&, juce::PropertiesFile&, Callbacks);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    collab::Session& session;
    juce::PropertiesFile& settings;
    Callbacks callbacks;
    juce::TextEditor portEditor, addressEditor;
    juce::TextButton hostButton { TRANS ("Host") }, joinButton { TRANS ("Join") }, leaveButton { TRANS ("Leave the session") }, copyButton { TRANS ("Copy") };
    juce::String invite;
};

//==============================================================================
/** The bottom line: the last message (click it for all of them), sync state, what's
    playing, CPU, sound card and version. */
class StatusBar  : public juce::Component,
                   public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    StatusBar (te::Engine&, collab::Session&);
    ~StatusBar() override;

    void setMessage (const juce::String&);
    void setPlaying (const juce::String&);
    std::function<void()> onShowLog;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    static constexpr int height = 26;

private:
    te::Engine& engine;
    collab::Session& session;
    juce::String message, playing, cpuText, deviceText;
    juce::Rectangle<int> messageArea;
    void timerCallback() override;
};

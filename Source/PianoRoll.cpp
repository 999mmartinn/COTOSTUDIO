#include "PianoRoll.h"

using namespace song;

//==============================================================================
class PianoRoll::Grid  : public juce::Component
{
public:
    explicit Grid (EditorState& s) : state (s)
    {
        setSize (600, (highestPitch - lowestPitch + 1) * rowHeight);
    }

    int pitchAtY (float y) const       { return juce::jlimit (lowestPitch, highestPitch, highestPitch - (int) (y / rowHeight)); }
    float yForPitch (int pitch) const   { return (float) ((highestPitch - pitch) * rowHeight); }

    double beatsPerPixel() const
    {
        auto pattern = state.getPattern();
        return getLengthBeats (pattern) / juce::jmax (1.0, (double) (getWidth() - keyboardWidth));
    }

    double beatAtX (float x) const     { return juce::jmax (0.0, (x - keyboardWidth) * beatsPerPixel()); }
    float xForBeat (double beat) const  { return keyboardWidth + (float) (beat / beatsPerPixel()); }
    static double snap (double beat)    { return std::floor (beat / stepBeats + 1.0e-6) * stepBeats; }

    static bool isBlackKey (int pitch)
    {
        const int n = pitch % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    void paint (juce::Graphics& g) override
    {
        auto pattern = state.getPattern();
        auto* channel = state.getSelectedChannel();
        const double length = getLengthBeats (pattern);
        const int numSteps = getNumSteps (pattern);
        const int playhead = state.getPlayheadStep();

        g.fillAll (juce::Colour (0xff23272d));

        // Rows and keyboard
        for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
        {
            const float y = yForPitch (pitch);
            const bool black = isBlackKey (pitch);
            g.setColour (black ? juce::Colour (0xff262b32) : juce::Colour (0xff2c323a));
            g.fillRect ((float) keyboardWidth, y, (float) (getWidth() - keyboardWidth), (float) rowHeight);

            g.setColour (black ? juce::Colour (0xff1a1a1a) : juce::Colour (0xffe8e8e8));
            g.fillRect (0.0f, y, (float) keyboardWidth - 2.0f, (float) rowHeight - 1.0f);

            if (pitch % 12 == 0)
            {
                g.setColour (juce::Colours::black);
                g.setFont (10.0f);
                g.drawText ("C" + juce::String (pitch / 12), 2, (int) y, keyboardWidth - 6, rowHeight,
                            juce::Justification::centredRight);
                g.setColour (juce::Colour (0xff3d4550));
                g.drawHorizontalLine ((int) (y + rowHeight) - 1, (float) keyboardWidth, (float) getWidth());
            }
        }

        // Vertical step lines
        for (int s = 0; s <= numSteps; ++s)
        {
            const float x = xForBeat (s * stepBeats);
            g.setColour (s % 16 == 0 ? juce::Colour (0xff6b7480) : s % 4 == 0 ? juce::Colour (0xff4a525d) : juce::Colour (0xff353c45));
            g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
        }

        if (playhead >= 0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.fillRect (juce::Rectangle<float> (xForBeat (playhead * stepBeats), 0.0f,
                                                xForBeat ((playhead + 1) * stepBeats) - xForBeat (playhead * stepBeats),
                                                (float) getHeight()));
        }

        if (channel == nullptr || ! pattern.isValid())
            return;

        const auto colour = EditorState::channelColour (getChannels (channel->edit).indexOf (channel));

        for (auto& n : getNotes (pattern, getChannelID (*channel)))
        {
            auto r = noteBounds (n);

            if (r.getX() >= xForBeat (length))
                continue;

            g.setColour (colour);
            g.fillRoundedRectangle (r.reduced (0.5f, 1.0f), 2.5f);
            g.setColour (colour.darker (0.6f));
            g.drawRoundedRectangle (r.reduced (0.5f, 1.0f), 2.5f, 1.0f);
        }
    }

    juce::Rectangle<float> noteBounds (const juce::ValueTree& n) const
    {
        const double b = n[ids::b], l = n[ids::l];
        const float x1 = xForBeat (b), x2 = xForBeat (b + l);
        return { x1, yForPitch ((int) n[ids::p]), juce::jmax (4.0f, x2 - x1), (float) rowHeight };
    }

    juce::ValueTree noteAt (juce::Point<float> p) const
    {
        auto pattern = state.getPattern();
        auto* channel = state.getSelectedChannel();

        if (channel == nullptr || ! pattern.isValid())
            return {};

        auto notes = getNotes (pattern, getChannelID (*channel));

        for (int i = notes.size(); --i >= 0;)
            if (noteBounds (notes[i]).contains (p))
                return notes[i];

        return {};
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto n = noteAt (e.position);
        const bool onEdge = n.isValid() && e.position.x > noteBounds (n).getRight() - 6.0f;
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                               : n.isValid() ? juce::MouseCursor::DraggingHandCursor
                                             : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();
        auto pattern = state.getPattern();
        auto* channel = state.getSelectedChannel();
        dragNote = {};

        if (edit == nullptr || channel == nullptr || ! pattern.isValid() || e.position.x < keyboardWidth)
            return;

        state.beginAction();
        auto hit = noteAt (e.position);

        if (e.mods.isPopupMenu())
        {
            if (hit.isValid())
                removeNote (*edit, hit);

            repaint();
            return;
        }

        if (hit.isValid())
        {
            dragNote = hit;
            dragMode = e.position.x > noteBounds (hit).getRight() - 6.0f ? DragMode::resize : DragMode::move;
            dragStartBeat = hit[ids::b];
            dragStartPitch = hit[ids::p];
            dragGrabBeat = beatAtX (e.position.x);
        }
        else
        {
            const double beat = snap (beatAtX (e.position.x));

            if (beat >= getLengthBeats (pattern))
                return;

            dragNote = addNote (*edit, pattern, getChannelID (*channel), pitchAtY (e.position.y), beat, stepBeats);
            dragMode = DragMode::resize;
            dragStartBeat = beat;
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();

        if (! dragNote.isValid() || edit == nullptr)
            return;

        auto* um = &edit->getUndoManager();
        const double length = getLengthBeats (state.getPattern());

        if (dragMode == DragMode::resize)
        {
            const double start = dragNote[ids::b];
            const double end = juce::jlimit (start + stepBeats, length, snap (beatAtX (e.position.x)) + stepBeats);

            if (std::abs (end - start - (double) dragNote[ids::l]) > 1.0e-6)
                dragNote.setProperty (ids::l, end - start, um);
        }
        else
        {
            const double noteLength = dragNote[ids::l];
            const double newStart = juce::jlimit (0.0, juce::jmax (0.0, length - stepBeats),
                                                  snap (dragStartBeat + beatAtX (e.position.x) - dragGrabBeat + stepBeats * 0.5));
            const int newPitch = juce::jlimit (lowestPitch, highestPitch,
                                               dragStartPitch + (pitchAtY (e.position.y) - pitchAtY (e.mouseDownPosition.y)));

            if (std::abs (newStart - (double) dragNote[ids::b]) > 1.0e-6)
                dragNote.setProperty (ids::b, newStart, um);

            if (newPitch != (int) dragNote[ids::p])
                dragNote.setProperty (ids::p, newPitch, um);

            juce::ignoreUnused (noteLength);
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragNote = {};
    }

    EditorState& state;
    juce::ValueTree dragNote;
    enum class DragMode { move, resize } dragMode = DragMode::move;
    double dragStartBeat = 0.0, dragGrabBeat = 0.0;
    int dragStartPitch = 60;
};

//==============================================================================
PianoRoll::PianoRoll (EditorState& s) : state (s)
{
    grid = std::make_unique<Grid> (state);
    viewport.setViewedComponent (grid.get(), false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    title.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    title.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (title);
    startTimerHz (30);
}

PianoRoll::~PianoRoll()
{
    stopTimer();
}

void PianoRoll::refresh()
{
    auto* channel = state.getSelectedChannel();
    auto pattern = state.getPattern();

    title.setText ("Piano roll - " + (channel != nullptr ? channel->getName() : juce::String ("no channel"))
                     + " in " + pattern[ids::name].toString()
                     + "   (click: add note, drag: move/longer, right-click: delete)",
                   juce::dontSendNotification);
    grid->repaint();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff2a2f36));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void PianoRoll::resized()
{
    auto r = getLocalBounds().reduced (4);
    title.setBounds (r.removeFromTop (20));
    viewport.setBounds (r);
    grid->setSize (viewport.getWidth() - viewport.getScrollBarThickness(), grid->getHeight());

    if (! scrolledOnce && viewport.getHeight() > 0)
    {
        // Centre on C5 (MIDI 60, FL naming), where steps put their notes.
        scrolledOnce = true;
        viewport.setViewPosition (0, juce::jmax (0, (int) grid->yForPitch (60) - viewport.getHeight() / 2));
    }
}

void PianoRoll::timerCallback()
{
    const int playhead = state.getPlayheadStep();

    if (playhead != lastPlayhead)
    {
        lastPlayhead = playhead;
        grid->repaint();
    }
}

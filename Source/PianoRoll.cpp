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

    /** The selected channel, if it plays notes (audio channels have none). */
    te::AudioTrack* getNoteChannel() const
    {
        auto* c = state.getSelectedChannel();
        return c != nullptr && ! isAudioChannel (*c) ? c : nullptr;
    }

    static bool isBlackKey (int pitch)
    {
        const int n = pitch % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }

    void paint (juce::Graphics& g) override
    {
        auto pattern = state.getPattern();
        auto* channel = getNoteChannel();
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

        juce::ignoreUnused (playhead);

        // Start marker (where Play starts)
        g.setColour (juce::Colour (0xff6fd06f).withAlpha (0.5f));
        g.drawVerticalLine ((int) xForBeat (state.getStartBeat()), 0.0f, (float) getHeight());

        if (channel != nullptr && pattern.isValid())
            paintNotes (g, pattern, *channel, length);

        // Playhead
        const double playBeat = state.getPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawVerticalLine ((int) xForBeat (playBeat), 0.0f, (float) getHeight());
        }
    }

    void paintNotes (juce::Graphics& g, const juce::ValueTree& pattern, te::AudioTrack& channel, double length)
    {

        const auto colour = EditorState::channelColour (getChannels (channel.edit).indexOf (&channel));

        for (auto& n : getNotes (pattern, getChannelID (channel)))
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
        auto* channel = getNoteChannel();

        if (channel == nullptr || ! pattern.isValid())
            return {};

        auto notes = getNotes (pattern, getChannelID (*channel));

        for (int i = notes.size(); --i >= 0;)
            if (noteBounds (notes[i]).contains (p))
                return notes[i];

        return {};
    }

    //==============================================================================
    // Note preview (local only, like FL): hear the note while you place or drag it.
    void startPreview (int pitch)
    {
        if (pitch == previewPitch)
            return;

        if (auto* channel = getNoteChannel())
        {
            channel->playGuideNote (pitch, te::MidiChannel (1), 100, true, true, false);
            previewPitch = pitch;
        }
    }

    void stopPreview()
    {
        if (previewPitch >= 0)
            if (auto* channel = getNoteChannel())
                channel->turnOffGuideNotes();

        previewPitch = -1;
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
        auto* channel = getNoteChannel();
        dragNote = {};

        if (edit == nullptr || channel == nullptr || ! pattern.isValid())
            return;

        if (e.position.x < keyboardWidth)
        {
            // Clicking the keyboard just plays the key.
            startPreview (pitchAtY (e.position.y));
            return;
        }

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
            startPreview (dragStartPitch);
        }
        else
        {
            const double beat = snap (beatAtX (e.position.x));

            if (beat >= getLengthBeats (pattern))
                return;

            dragNote = addNote (*edit, pattern, getChannelID (*channel), pitchAtY (e.position.y), beat, stepBeats);
            dragMode = DragMode::resize;
            dragStartBeat = beat;
            startPreview (pitchAtY (e.position.y));
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();

        if (e.mouseDownPosition.x < keyboardWidth && ! dragNote.isValid())
        {
            startPreview (pitchAtY (e.position.y));   // glide over the keys
            return;
        }

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
            {
                dragNote.setProperty (ids::p, newPitch, um);
                startPreview (newPitch);
            }

            juce::ignoreUnused (noteLength);
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragNote = {};
        stopPreview();
    }

    EditorState& state;
    juce::ValueTree dragNote;
    enum class DragMode { move, resize } dragMode = DragMode::move;
    double dragStartBeat = 0.0, dragGrabBeat = 0.0;
    int dragStartPitch = 60, previewPitch = -1;
};

//==============================================================================
/** Time ruler above the piano roll: click or drag to set where Play starts.
    While playing, clicking jumps there (for everyone). */
class PianoRoll::Ruler  : public juce::Component
{
public:
    Ruler (EditorState& s, Grid& g) : state (s), grid (g) {}

    void paint (juce::Graphics& g) override
    {
        auto pattern = state.getPattern();
        const int numSteps = getNumSteps (pattern);

        g.fillAll (juce::Colour (0xff1c2025));

        for (int s = 0; s <= numSteps; ++s)
        {
            const float x = grid.xForBeat (s * stepBeats);

            if (s % 4 == 0)
            {
                g.setColour (juce::Colour (s % 16 == 0 ? 0xffa0a8b4 : 0xff5a626d));
                g.drawVerticalLine ((int) x, s % 16 == 0 ? 2.0f : 10.0f, (float) getHeight());

                if (s < numSteps)
                {
                    g.setFont (11.0f);
                    g.setColour (juce::Colours::white.withAlpha (s % 16 == 0 ? 0.9f : 0.45f));
                    g.drawText (juce::String (s / 16 + 1) + (s % 16 == 0 ? juce::String() : "." + juce::String ((s % 16) / 4 + 1)),
                                (int) x + 3, 0, 40, getHeight(), juce::Justification::centredLeft);
                }
            }
        }

        // Start marker
        const float mx = grid.xForBeat (state.getStartBeat());
        juce::Path tri;
        tri.addTriangle (mx - 6.0f, 2.0f, mx + 6.0f, 2.0f, mx, (float) getHeight() - 2.0f);
        g.setColour (juce::Colour (0xff6fd06f));
        g.fillPath (tri);

        const double playBeat = state.getPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (juce::Colours::white);
            g.drawVerticalLine ((int) grid.xForBeat (playBeat), 0.0f, (float) getHeight());
        }

        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (10.0f);
        g.drawText ("start", 4, 0, keyboardWidth - 8, getHeight(), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override    { setStart (e.position.x, true); }
    void mouseDrag (const juce::MouseEvent& e) override    { setStart (e.position.x, false); }

    void setStart (float x, bool seekIfPlaying)
    {
        if (x < keyboardWidth)
            x = (float) keyboardWidth;

        state.startBeat = Grid::snap (grid.beatAtX (x));
        state.startBeat = state.getStartBeat();   // keep it inside the pattern

        if (seekIfPlaying)
            if (auto* edit = state.getEdit())
                if (edit->getTransport().isPlaying())
                    state.session.seek (state.beatsToSeconds (state.startBeat));

        repaint();
        grid.repaint();
    }

private:
    EditorState& state;
    Grid& grid;
};

//==============================================================================
PianoRoll::PianoRoll (EditorState& s) : state (s)
{
    grid = std::make_unique<Grid> (state);
    ruler = std::make_unique<Ruler> (state, *grid);
    addAndMakeVisible (*ruler);
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

    if (channel != nullptr && song::isAudioChannel (*channel))
        title.setText ("Piano roll - " + channel->getName() + " is an audio channel: move and trim its clips in the playlist",
                       juce::dontSendNotification);
    else
        title.setText ("Piano roll - " + (channel != nullptr ? channel->getName() : juce::String ("no channel"))
                         + " in " + pattern[ids::name].toString()
                         + "   (click: add note, drag: move/longer, right-click: delete, ruler: set play start)",
                       juce::dontSendNotification);
    grid->repaint();
    ruler->repaint();
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
    ruler->setBounds (r.removeFromTop (18).withTrimmedRight (viewport.getScrollBarThickness()));
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
    // Smooth playhead while playing; one last repaint when it stops.
    const int playhead = state.getPlayheadStep();

    if (playhead >= 0 || playhead != lastPlayhead)
    {
        lastPlayhead = playhead;
        grid->repaint (viewport.getViewArea());
        ruler->repaint();
    }
}

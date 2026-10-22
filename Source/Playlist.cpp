#include "Playlist.h"

using namespace song;

static juce::Colour patternColour (const juce::ValueTree& pattern)
{
    auto c = pattern[ids::colour].toString();
    return c.isNotEmpty() ? juce::Colour::fromString (c) : juce::Colour (0xff5fb85f);
}

//==============================================================================
class Playlist::ScrollViewport  : public juce::Viewport
{
public:
    std::function<void()> onScroll;
    void visibleAreaChanged (const juce::Rectangle<int>&) override   { if (onScroll) onScroll(); }
};

//==============================================================================
class Playlist::Lanes  : public juce::Component
{
public:
    Lanes (Playlist& p) : owner (p), state (p.state) {}

    float xForBeat (double beat) const   { return (float) (beat * owner.pixelsPerBeat); }
    double beatAtX (float x) const       { return juce::jmax (0.0, (double) x / owner.pixelsPerBeat); }
    int laneAtY (float y) const          { return juce::jlimit (0, numLanes - 1, (int) (y / laneHeight)); }
    double snap (double beat) const      { return std::floor (beat / state.playlistSnapBeats + 1.0e-6) * state.playlistSnapBeats; }

    juce::Rectangle<float> clipBounds (const juce::ValueTree& clip) const
    {
        auto* edit = state.getEdit();
        const double start = clip[ids::start];
        const double len = edit != nullptr ? getClipLength (*edit, clip) : 4.0;
        return { xForBeat (start), (float) ((int) clip[ids::lane] * laneHeight), xForBeat (start + len) - xForBeat (start),
                 (float) laneHeight };
    }

    juce::ValueTree clipAt (juce::Point<float> p) const
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return {};

        auto clips = getClips (*edit);

        for (int i = clips.size(); --i >= 0;)
            if (clipBounds (clips[i]).contains (p))
                return clips[i];

        return {};
    }

    void paint (juce::Graphics& g) override
    {
        auto* edit = state.getEdit();
        g.fillAll (juce::Colour (0xff23272d));

        if (edit == nullptr)
            return;

        auto visible = g.getClipBounds();

        for (int lane = 0; lane < numLanes; ++lane)
        {
            g.setColour (lane % 2 == 0 ? juce::Colour (0xff2a3038) : juce::Colour (0xff262b32));
            g.fillRect (0, lane * laneHeight, getWidth(), laneHeight);
        }

        // Beat and bar lines
        const int firstBeat = juce::jmax (0, (int) beatAtX ((float) visible.getX()) - 1);
        const int lastBeat = (int) beatAtX ((float) visible.getRight()) + 1;

        for (int b = firstBeat; b <= lastBeat; ++b)
        {
            g.setColour (b % 4 == 0 ? juce::Colour (0xff4f5864) : juce::Colour (0xff323942));
            g.drawVerticalLine ((int) xForBeat (b), 0.0f, (float) getHeight());
        }

        // Song end
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawVerticalLine ((int) xForBeat (getSongLengthBeats (*edit)), 0.0f, (float) getHeight());

        // Clips
        const auto viewedSid = state.getPattern()[ids::sid].toString();

        for (auto& clip : getClips (*edit))
        {
            auto pattern = findPattern (*edit, clip[ids::pattern].toString());
            auto r = clipBounds (clip).reduced (0.5f, 1.5f);

            if (! r.intersects (visible.toFloat()))
                continue;

            const auto colour = pattern.isValid() ? patternColour (pattern) : juce::Colours::grey;
            g.setColour (colour.withAlpha (0.35f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (colour);
            g.drawRoundedRectangle (r, 3.0f, pattern[ids::sid].toString() == viewedSid ? 2.0f : 1.0f);

            if (! pattern.isValid())
                continue;

            // Mini preview, FL-style: one thin row per channel that has notes in this pattern.
            const double clipLen = getClipLength (*edit, clip);
            juce::StringArray rows;

            for (auto* t : getChannels (*edit))
                if (! getNotes (pattern, getChannelID (*t)).isEmpty())
                    rows.add (getChannelID (*t));

            if (! rows.isEmpty())
            {
                const float top = r.getY() + 13.0f, h = r.getHeight() - 15.0f;
                const float rowH = h / (float) rows.size();
                g.setColour (colour.brighter (0.5f));

                for (auto n : pattern)
                {
                    if (! n.hasType (ids::PNOTE) || (double) n[ids::b] >= clipLen)
                        continue;

                    const int row = rows.indexOf (n[ids::ch].toString());

                    if (row < 0)
                        continue;

                    const float x = r.getX() + xForBeat (n[ids::b]);
                    const float w = juce::jmax (1.5f, xForBeat (juce::jmin ((double) n[ids::l], clipLen - (double) n[ids::b])) - 1.0f);
                    g.fillRect (x, top + row * rowH, w, juce::jmax (1.0f, rowH - 1.0f));
                }
            }

            g.setColour (juce::Colours::white);
            g.setFont (11.0f);
            g.drawText (pattern[ids::name].toString(), r.reduced (4.0f, 1.0f).withHeight (12.0f).toNearestInt(),
                        juce::Justification::centredLeft, true);
        }

        // Start marker and playhead
        g.setColour (juce::Colour (0xff6fd06f).withAlpha (0.5f));
        g.drawVerticalLine ((int) xForBeat (state.songStartBeat), 0.0f, (float) getHeight());

        const double playBeat = state.getSongPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawVerticalLine ((int) xForBeat (playBeat), 0.0f, (float) getHeight());
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto c = clipAt (e.position);
        const bool onEdge = c.isValid() && e.position.x > clipBounds (c).getRight() - 6.0f;
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                               : c.isValid() ? juce::MouseCursor::DraggingHandCursor
                                             : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();
        dragClip = {};
        moved = false;

        if (edit == nullptr)
            return;

        state.beginAction();
        auto hit = clipAt (e.position);

        if (e.mods.isPopupMenu())
        {
            if (hit.isValid())
                removeClip (*edit, hit);

            repaint();
            return;
        }

        if (hit.isValid())
        {
            dragClip = hit;
            dragMode = e.position.x > clipBounds (hit).getRight() - 6.0f ? DragMode::resize : DragMode::move;
            grabStart = hit[ids::start];
            grabBeat = beatAtX (e.position.x);
            clickedExisting = true;
        }
        else
        {
            auto pattern = state.getPattern();

            if (! pattern.isValid())
                return;

            dragClip = addClip (*edit, pattern[ids::sid].toString(), laneAtY (e.position.y), snap (beatAtX (e.position.x)));
            dragMode = DragMode::move;
            grabStart = dragClip[ids::start];
            grabBeat = beatAtX (e.position.x);
            clickedExisting = false;
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || ! dragClip.isValid() || e.getDistanceFromDragStart() < 3)
            return;

        moved = true;
        auto* um = &edit->getUndoManager();

        if (dragMode == DragMode::move)
        {
            const double newStart = juce::jmax (0.0, snap (grabStart + beatAtX (e.position.x) - grabBeat + state.playlistSnapBeats * 0.5));
            const int newLane = laneAtY (e.position.y);

            if (std::abs (newStart - (double) dragClip[ids::start]) > 1.0e-6)
                dragClip.setProperty (ids::start, newStart, um);

            if (newLane != (int) dragClip[ids::lane])
                dragClip.setProperty (ids::lane, newLane, um);
        }
        else
        {
            const double start = dragClip[ids::start];
            const double end = juce::jmax (start + stepBeats, snap (beatAtX (e.position.x) + state.playlistSnapBeats * 0.5));

            if (std::abs (end - start - getClipLength (*edit, dragClip)) > 1.0e-6)
                dragClip.setProperty (ids::length, end - start, um);
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // A click (no drag) on a clip opens its pattern for editing.
        if (dragClip.isValid() && clickedExisting && ! moved)
        {
            state.viewedPattern = dragClip[ids::pattern].toString();

            if (state.onSelectionChanged)
                state.onSelectionChanged();
        }

        dragClip = {};
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown())
            owner.setZoom (owner.pixelsPerBeat * (w.deltaY > 0 ? 1.15f : 1.0f / 1.15f), e.position.x);
        else
            Component::mouseWheelMove (e, w);
    }

private:
    Playlist& owner;
    EditorState& state;
    juce::ValueTree dragClip;
    enum class DragMode { move, resize } dragMode = DragMode::move;
    double grabStart = 0.0, grabBeat = 0.0;
    bool moved = false, clickedExisting = false;
};

//==============================================================================
class Playlist::Ruler  : public juce::Component
{
public:
    Ruler (Playlist& p) : owner (p), state (p.state) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c2025));
        const int scrollX = owner.viewport->getViewPositionX();
        const float ppb = owner.pixelsPerBeat;
        const int firstBar = juce::jmax (0, (int) (scrollX / ppb / 4.0f));
        const int lastBar = (int) ((scrollX + getWidth()) / ppb / 4.0f) + 1;
        const int labelEvery = ppb < 8.0f ? 4 : ppb < 14.0f ? 2 : 1;

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            const float x = bar * 4.0f * ppb - (float) scrollX;
            g.setColour (juce::Colour (0xff8a939e));
            g.drawVerticalLine ((int) x, 6.0f, (float) getHeight());

            if (bar % labelEvery == 0)
            {
                g.setFont (11.0f);
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawText (juce::String (bar + 1), (int) x + 3, 0, 40, getHeight(), juce::Justification::centredLeft);
            }
        }

        const float mx = (float) (state.songStartBeat * ppb) - (float) scrollX;
        juce::Path tri;
        tri.addTriangle (mx - 6.0f, 2.0f, mx + 6.0f, 2.0f, mx, (float) getHeight() - 2.0f);
        g.setColour (juce::Colour (0xff6fd06f));
        g.fillPath (tri);

        const double playBeat = state.getSongPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (juce::Colours::white);
            g.drawVerticalLine ((int) (playBeat * ppb) - scrollX, 0.0f, (float) getHeight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override   { setStart (e.position.x, true); }
    void mouseDrag (const juce::MouseEvent& e) override   { setStart (e.position.x, false); }

    void setStart (float x, bool seekIfPlaying)
    {
        const double beat = juce::jmax (0.0, (double) (x + (float) owner.viewport->getViewPositionX()) / owner.pixelsPerBeat);
        state.songStartBeat = std::floor (beat / state.playlistSnapBeats + 1.0e-6) * state.playlistSnapBeats;

        if (seekIfPlaying && state.session.isSongMode())
            if (auto* edit = state.getEdit())
                if (edit->getTransport().isPlaying())
                    state.session.seek (state.beatsToSeconds (state.songStartBeat));

        owner.repaint();
        owner.lanes->repaint();
    }

private:
    Playlist& owner;
    EditorState& state;
};

//==============================================================================
class Playlist::LaneHeader  : public juce::Component
{
public:
    LaneHeader (Playlist& p) : owner (p) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c2025));
        const int scrollY = owner.viewport->getViewPositionY();
        g.setFont (11.0f);

        for (int lane = 0; lane < numLanes; ++lane)
        {
            const int y = lane * laneHeight - scrollY;
            g.setColour (lane % 2 == 0 ? juce::Colour (0xff2a3038) : juce::Colour (0xff262b32));
            g.fillRect (0, y, getWidth() - 2, laneHeight - 1);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawText ("Track " + juce::String (lane + 1), 6, y, getWidth() - 8, laneHeight, juce::Justification::centredLeft);
        }
    }

private:
    Playlist& owner;
};

//==============================================================================
Playlist::Playlist (EditorState& s) : state (s)
{
    lanes = std::make_unique<Lanes> (*this);
    ruler = std::make_unique<Ruler> (*this);
    header = std::make_unique<LaneHeader> (*this);
    viewport = std::make_unique<ScrollViewport>();

    viewport->setViewedComponent (lanes.get(), false);
    viewport->setScrollBarsShown (true, true);
    viewport->onScroll = [this] { ruler->repaint(); header->repaint(); };

    addAndMakeVisible (*viewport);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*header);

    title.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    title.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (title);

    snapBox.addItem ("Snap: bar", 1);
    snapBox.addItem ("Snap: beat", 2);
    snapBox.addItem ("Snap: step", 3);
    snapBox.setSelectedId (1, juce::dontSendNotification);
    snapBox.onChange = [this]
    {
        state.playlistSnapBeats = snapBox.getSelectedId() == 1 ? 4.0 : snapBox.getSelectedId() == 2 ? 1.0 : stepBeats;
    };
    addAndMakeVisible (snapBox);

    startTimerHz (30);
}

Playlist::~Playlist()
{
    stopTimer();
}

void Playlist::refresh()
{
    title.setText ("Playlist - click to place \"" + state.getPattern()[ids::name].toString()
                     + "\", drag to move, drag right edge to resize, right-click to delete, click a clip to edit it, Ctrl+wheel to zoom",
                   juce::dontSendNotification);
    layoutContent();
    lanes->repaint();
    ruler->repaint();
}

void Playlist::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff2a2f36));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void Playlist::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto top = r.removeFromTop (22);
    snapBox.setBounds (top.removeFromRight (110));
    title.setBounds (top);
    r.removeFromTop (2);

    auto rulerRow = r.removeFromTop (rulerHeight);
    rulerRow.removeFromLeft (laneHeaderWidth);
    ruler->setBounds (rulerRow.withTrimmedRight (viewport->getScrollBarThickness()));

    header->setBounds (r.removeFromLeft (laneHeaderWidth).withTrimmedBottom (viewport->getScrollBarThickness()));
    viewport->setBounds (r);
    layoutContent();
}

void Playlist::layoutContent()
{
    auto* edit = state.getEdit();
    const double songBeats = edit != nullptr ? getSongLengthBeats (*edit) : 4.0;
    const double shownBeats = juce::jmax (64.0 * 4.0, songBeats + 16.0 * 4.0);
    lanes->setSize (juce::jmax (viewport->getMaximumVisibleWidth(), (int) (shownBeats * pixelsPerBeat)),
                    numLanes * laneHeight);
}

void Playlist::setZoom (float newPixelsPerBeat, float anchorX)
{
    newPixelsPerBeat = juce::jlimit (4.0f, 120.0f, newPixelsPerBeat);
    const double anchorBeat = anchorX / pixelsPerBeat;
    const float anchorOnScreen = anchorX - (float) viewport->getViewPositionX();

    pixelsPerBeat = newPixelsPerBeat;
    layoutContent();
    viewport->setViewPosition (juce::jmax (0, (int) (anchorBeat * pixelsPerBeat - anchorOnScreen)), viewport->getViewPositionY());
    lanes->repaint();
    ruler->repaint();
}

void Playlist::timerCallback()
{
    auto* edit = state.getEdit();
    const bool playing = edit != nullptr && edit->getTransport().isPlaying() && state.session.isSongMode();

    if (playing || playing != wasPlaying)
    {
        wasPlaying = playing;
        lanes->repaint (viewport->getViewArea());
        ruler->repaint();
    }
}

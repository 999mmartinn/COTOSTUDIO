#include "PianoRoll.h"
#include "FileImport.h"
#include "Scales.h"

using namespace song;

//==============================================================================
class PianoRoll::Grid  : public juce::Component,
                        public juce::DragAndDropTarget,
                        public juce::FileDragAndDropTarget
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

    /** With the scale helper on, the scale note nearest to a pitch (otherwise the pitch). */
    int snapPitch (int pitch, bool preferUp = false) const
    {
        if (! state.scaleSnap)
            return pitch;

        return scales::nearest (state.scaleKey, state.scaleType, pitch, preferUp, lowestPitch, highestPitch);
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

        g.fillAll (theme::colours::lane2);

        // Rows and keyboard
        for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
        {
            const float y = yForPitch (pitch);
            const bool black = isBlackKey (pitch);
            const bool inScale = ! state.scaleSnap || scales::contains (state.scaleKey, state.scaleType, pitch);
            const auto row = juce::Rectangle<float> ((float) keyboardWidth, y, (float) (getWidth() - keyboardWidth), (float) rowHeight);

            if (state.scaleSnap)
            {
                // Scale rows light, the others dark; the key note's rows tinted.
                g.setColour (inScale ? theme::colours::lane1 : theme::colours::bg);
                g.fillRect (row);

                if (scales::isKeyNote (state.scaleKey, pitch))
                {
                    g.setColour (theme::colours::accent.withAlpha (0.13f));
                    g.fillRect (row);
                }
            }
            else
            {
                g.setColour (black ? theme::colours::lane2 : theme::colours::lane1);
                g.fillRect (row);
            }

            // Keys: light lilac-white and deep purple-black.
            g.setColour (black ? juce::Colour (0xff120f19) : juce::Colour (0xffe6e0f4));
            g.fillRect (0.0f, y, (float) keyboardWidth - 2.0f, (float) rowHeight - 1.0f);

            if (state.scaleSnap && inScale)
            {
                // A dot on the keys of the scale (lilac on the key note).
                const bool keyNote = scales::isKeyNote (state.scaleKey, pitch);
                g.setColour (keyNote ? theme::colours::accent : (black ? juce::Colour (0xffb9b0cc) : juce::Colour (0xff6f6585)));
                g.fillEllipse (juce::Rectangle<float> (keyNote ? 6.0f : 4.0f, keyNote ? 6.0f : 4.0f)
                                 .withCentre ({ 9.0f, y + rowHeight * 0.5f - 0.5f }));
            }

            if (pitch % 12 == 0)
            {
                g.setColour (theme::colours::accentText.withAlpha (0.8f));
                g.setFont (theme::mono (9.5f, true));
                g.drawText ("C" + juce::String (pitch / 12), 2, (int) y, keyboardWidth - 6, rowHeight,
                            juce::Justification::centredRight);
                g.setColour (theme::colours::gridBar);
                g.drawHorizontalLine ((int) (y + rowHeight) - 1, (float) keyboardWidth, (float) getWidth());
            }
        }

        // Vertical step lines
        for (int s = 0; s <= numSteps; ++s)
        {
            const float x = xForBeat (s * stepBeats);
            g.setColour (s % 16 == 0 ? theme::colours::gridBar.brighter (0.25f) : s % 4 == 0 ? theme::colours::gridBar : theme::colours::grid);
            g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
        }

        juce::ignoreUnused (playhead);

        // Start marker (where Play starts)
        g.setColour (theme::colours::green.withAlpha (0.55f));
        g.drawVerticalLine ((int) xForBeat (state.getStartBeat()), 0.0f, (float) getHeight());

        if (channel != nullptr && pattern.isValid())
            paintNotes (g, pattern, *channel, length);

        // Playhead
        const double playBeat = state.getPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (theme::colours::text);
            g.fillRect (juce::Rectangle<float> (xForBeat (playBeat) - 1.0f, 0.0f, 2.0f, (float) getHeight()));
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
            g.fillRoundedRectangle (r.reduced (0.5f, 1.0f), 3.0f);
            g.setColour (colour.darker (0.7f));
            g.drawRoundedRectangle (r.reduced (0.5f, 1.0f), 3.0f, 1.0f);
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

            const int pitch = snapPitch (pitchAtY (e.position.y));
            dragNote = addNote (*edit, pattern, getChannelID (*channel), pitch, beat, stepBeats);
            dragMode = DragMode::resize;
            dragStartBeat = beat;
            startPreview (pitch);
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
            const int rows = pitchAtY (e.position.y) - pitchAtY (e.mouseDownPosition.y);
            int newPitch = juce::jlimit (lowestPitch, highestPitch, dragStartPitch + rows);

            // Scale helper: moved up or down, the note lands on a scale note (left
            // where it is while only moved in time, even if it is off the scale).
            if (rows != 0)
                newPitch = snapPitch (newPitch, rows > 0);

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

    //==============================================================================
    // A MIDI file dropped here (sound browser or Explorer): its notes replace the
    // selected channel's notes in this pattern.
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        return fileimport::isMidiFile (fileimport::draggedFile (d.description));
    }

    void itemDragEnter (const SourceDetails& d) override   { setMidiDrop (fileimport::draggedFile (d.description)); }
    void itemDragExit (const SourceDetails&) override      { setMidiDrop ({}); }

    void itemDropped (const SourceDetails& d) override
    {
        setMidiDrop ({});
        importMidi (fileimport::draggedFile (d.description));
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (fileimport::isMidiFile (juce::File (f)))
                return true;

        return false;
    }

    void fileDragEnter (const juce::StringArray& files, int, int) override
    {
        for (auto& f : files)
            if (fileimport::isMidiFile (juce::File (f)))
                setMidiDrop (juce::File (f));
    }

    void fileDragExit (const juce::StringArray&) override  { setMidiDrop ({}); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setMidiDrop ({});

        for (auto& f : files)
            if (fileimport::isMidiFile (juce::File (f)))
            {
                importMidi (juce::File (f));
                return;   // one file per channel
            }
    }

    void setMidiDrop (const juce::File& f)
    {
        midiDrop = f;
        repaint();
    }

    void importMidi (const juce::File& f)
    {
        auto* edit = state.getEdit();
        auto* channel = getNoteChannel();

        if (edit == nullptr)
            return;

        state.beginAction();
        auto channelID = channel != nullptr ? getChannelID (*channel) : juce::String();
        state.session.logMessage (fileimport::applyMidiFile (*edit, state.getPattern(), f, channelID));
        state.selectedChannel = channelID;

        if (state.onSelectionChanged)
            state.onSelectionChanged();
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (midiDrop == juce::File())
            return;

        auto area = getLocalBounds();

        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            area = vp->getViewArea();

        auto* channel = getNoteChannel();
        const auto text = TRANS ("Notes from") + " " + midiDrop.getFileName() + " " + TRANS ("into") + " "
                           + (channel != nullptr ? channel->getName() : TRANS ("a new Synth channel"));
        const auto colour = theme::colours::accent;
        g.setColour (theme::colours::accentSoft.withAlpha (0.6f));
        g.fillRect (area);
        g.setColour (colour);
        g.drawRect (area, 2);
        g.setColour (theme::colours::text);
        g.setFont (theme::ui (15.0f, theme::Weight::bold));
        g.drawText (text, area.reduced (12), juce::Justification::centred, true);
    }

    juce::File midiDrop;

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

        g.fillAll (theme::colours::panel);
        g.setColour (theme::colours::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        for (int s = 0; s <= numSteps; ++s)
        {
            const float x = grid.xForBeat (s * stepBeats);

            if (s % 4 == 0)
            {
                g.setColour (s % 16 == 0 ? theme::colours::muted : theme::colours::gridBar);
                g.drawVerticalLine ((int) x, s % 16 == 0 ? 2.0f : 10.0f, (float) getHeight());

                if (s < numSteps)
                {
                    g.setFont (theme::mono (10.0f, s % 16 == 0));
                    g.setColour (s % 16 == 0 ? theme::colours::text : theme::colours::dim);
                    g.drawText (juce::String (s / 16 + 1) + (s % 16 == 0 ? juce::String() : "." + juce::String ((s % 16) / 4 + 1)),
                                (int) x + 3, 0, 40, getHeight(), juce::Justification::centredLeft);
                }
            }
        }

        // Start marker
        const float mx = grid.xForBeat (state.getStartBeat());
        juce::Path tri;
        tri.addTriangle (mx - 6.0f, 2.0f, mx + 6.0f, 2.0f, mx, (float) getHeight() - 2.0f);
        g.setColour (theme::colours::green);
        g.fillPath (tri);

        const double playBeat = state.getPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (theme::colours::text);
            g.fillRect (grid.xForBeat (playBeat) - 1.0f, 0.0f, 2.0f, (float) getHeight());
        }

        g.setColour (theme::colours::dim);
        g.setFont (theme::ui (10.0f));
        g.drawText (TRANS ("start"), 4, 0, keyboardWidth - 8, getHeight(), juce::Justification::centredLeft);
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

    title.setColour (juce::Label::textColourId, theme::colours::muted);
    title.setFont (theme::ui (11.0f));
    title.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (title);

    // Scale helper (off by default: notes go anywhere).
    theme::setIcon (scaleButton, theme::Icon::note);
    theme::setStyle (scaleButton, "toggle");
    scaleButton.setClickingTogglesState (false);
    scaleButton.onClick = [this] { showScaleMenu(); };
    addAndMakeVisible (scaleButton);
    updateScaleButton();

    // Own window / full screen (icon buttons).
    for (auto* b : { &detachButton, &fullScreenButton })
    {
        b->getProperties().set ("iconOnly", true);
        addAndMakeVisible (b);
    }

    detachButton.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
    fullScreenButton.onClick = [this] { if (onFullScreenToggle) onFullScreenToggle(); };
    setDetached (false);
    setFullScreenState (false);
    startTimerHz (30);
}

void PianoRoll::setDetached (bool isDetached)
{
    detached = isDetached;
    theme::setIcon (detachButton, detached ? theme::Icon::attach : theme::Icon::detach);
    detachButton.setTooltip (detached ? TRANS ("Put the piano roll back in the main window (closing its window does the same)")
                                      : TRANS ("Give the piano roll a window of its own - e.g. on your second monitor"));
    setFullScreenState (false);
}

void PianoRoll::setFullScreenState (bool isFullScreen)
{
    theme::setIcon (fullScreenButton, isFullScreen ? theme::Icon::exitFullScreen : theme::Icon::fullScreen);
    fullScreenButton.setTooltip (isFullScreen ? TRANS ("Back to the normal size (Esc)")
                                              : detached ? TRANS ("Fill the whole screen with the piano roll")
                                                         : TRANS ("Make the piano roll fill the window (Esc goes back)"));
    fullScreenButton.repaint();
}

void PianoRoll::updateScaleButton()
{
    scaleButton.setButtonText (state.scaleSnap ? scales::describe (state.scaleKey, state.scaleType) : TRANS ("Scale"));
    scaleButton.setToggleState (state.scaleSnap, juce::dontSendNotification);
    scaleButton.setTooltip (state.scaleSnap
                              ? TRANS ("Notes you place or move up/down land on {SCALE} (the shaded rows are outside it). Click to change or turn it off")
                                  .replace ("{SCALE}", scales::describe (state.scaleKey, state.scaleType))
                              : TRANS ("Scale helper: pick a key and a scale, and the notes you place land only on its notes (handy for chords in key). Off: notes go anywhere"));
    resized();
}

void PianoRoll::showScaleMenu()
{
    juce::PopupMenu m, keys, types;

    for (int k = 0; k < 12; ++k)
        keys.addItem (100 + k, scales::keyName (k), true, k == state.scaleKey);

    for (int t = 0; t < scales::numTypes; ++t)
    {
        if (t == 9)
            types.addSeparator();

        types.addItem (200 + t, TRANS (scales::types[t].name), true, t == state.scaleType);
    }

    m.addSectionHeader (TRANS ("Scale helper (only on your screen)"));
    m.addItem (1, TRANS ("Off - place notes anywhere"), true, ! state.scaleSnap);
    m.addItem (2, TRANS ("On - keep notes in") + " " + scales::describe (state.scaleKey, state.scaleType), true, state.scaleSnap);
    m.addSeparator();
    m.addSubMenu (TRANS ("Key") + ": " + scales::keyName (state.scaleKey), keys);
    m.addSubMenu (TRANS ("Scale") + ": " + TRANS (scales::type (state.scaleType).name), types);
    m.addSeparator();

    auto* channel = state.getSelectedChannel();
    const bool canFit = state.getEdit() != nullptr && channel != nullptr && ! song::isAudioChannel (*channel);
    m.addItem (3, TRANS ("Move this channel's notes into") + " " + scales::describe (state.scaleKey, state.scaleType), canFit);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (scaleButton),
                     [safe = juce::Component::SafePointer<PianoRoll> (this)] (int r)
    {
        if (safe == nullptr || r == 0)
            return;

        auto& st = safe->state;

        if (r == 1)                     st.scaleSnap = false;
        else if (r == 2)                st.scaleSnap = true;
        else if (r >= 100 && r < 112)   { st.scaleKey = r - 100;  st.scaleSnap = true; }
        else if (r >= 200)              { st.scaleType = r - 200; st.scaleSnap = true; }
        else if (r == 3)
        {
            // Every note of this channel in the viewed pattern onto the nearest scale note.
            auto* edit = st.getEdit();
            auto* ch = st.getSelectedChannel();

            if (edit != nullptr && ch != nullptr)
            {
                st.beginAction();
                int moved = 0;

                for (auto& n : getNotes (st.getPattern(), getChannelID (*ch)))
                {
                    const int pitch = n[ids::p];
                    const int target = scales::nearest (st.scaleKey, st.scaleType, pitch, false, lowestPitch, highestPitch);

                    if (target != pitch)
                    {
                        n.setProperty (ids::p, target, &edit->getUndoManager());
                        ++moved;
                    }
                }

                st.session.logMessage (TRANS ("{N} note(s) of {CHANNEL} moved into {SCALE} (Ctrl+Z undoes it)")
                                         .replace ("{N}", juce::String (moved)).replace ("{CHANNEL}", ch->getName())
                                         .replace ("{SCALE}", scales::describe (st.scaleKey, st.scaleType)));
            }
        }

        safe->updateScaleButton();
        safe->grid->repaint();

        if (safe->onScaleChanged)
            safe->onScaleChanged();
    });
}

PianoRoll::~PianoRoll()
{
    stopTimer();
}

void PianoRoll::refresh()
{
    auto* channel = state.getSelectedChannel();
    auto pattern = state.getPattern();

    heading = channel != nullptr ? channel->getName() : TRANS ("no channel");
    headingColour = channel != nullptr && state.getEdit() != nullptr
                      ? EditorState::channelColour (song::getChannels (*state.getEdit()).indexOf (channel))
                      : theme::colours::muted;

    if (channel != nullptr && song::isAudioChannel (*channel))
        title.setText (TRANS ("is an audio channel: move and trim its clips in the playlist"), juce::dontSendNotification);
    else
        title.setText (TRANS ("in") + " " + pattern[ids::name].toString() + juce::String::fromUTF8 ("  \xc2\xb7  ")
                         + TRANS ("click: add note, drag: move/longer, right-click: delete, ruler: set play start"),
                       juce::dontSendNotification);

    resized();
    repaint (getLocalBounds().removeFromTop (titleHeight));
    grid->repaint();
    ruler->repaint();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (theme::colours::wellAlt);

    // Which channel: a colour chip and its name, then the hint.
    auto top = getLocalBounds().removeFromTop (titleHeight).reduced (12, 0);
    g.setColour (headingColour);
    g.fillRoundedRectangle (top.removeFromLeft (10).toFloat().withSizeKeepingCentre (10.0f, 10.0f), 3.0f);
    top.removeFromLeft (8);
    theme::drawPanelTitle (g, top, heading);
}

void PianoRoll::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (titleHeight).reduced (12, 0);

    // Right side of the title row: Scale, own window, full screen.
    auto buttons = top.reduced (0, 3);
    fullScreenButton.setBounds (buttons.removeFromRight (30));
    buttons.removeFromRight (4);
    detachButton.setBounds (buttons.removeFromRight (30));
    buttons.removeFromRight (8);
    const int scaleWidth = (int) juce::GlyphArrangement::getStringWidth (theme::ui (13.0f, theme::Weight::semibold), scaleButton.getButtonText()) + 15 + 6 + 26;
    scaleButton.setBounds (buttons.removeFromRight (juce::jmin (220, scaleWidth)));
    top.setRight (scaleButton.getX() - 8);

    const int headingWidth = 18 + (int) juce::GlyphArrangement::getStringWidth (theme::ui (12.5f, theme::Weight::bold), heading) + 8;
    title.setBounds (top.withTrimmedLeft (headingWidth));
    ruler->setBounds (r.removeFromTop (20).withTrimmedRight (viewport.getScrollBarThickness()));
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

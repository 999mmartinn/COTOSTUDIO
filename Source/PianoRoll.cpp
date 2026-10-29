#include "PianoRoll.h"
#include "FileImport.h"
#include "Scales.h"

using namespace song;

namespace
{
    const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    /** Notes copied with Ctrl+C / Ctrl+X (this PC only). Ctrl+V puts them back at the same
        beats, in whichever pattern and channel you are on then (like FL). */
    struct CopiedNote { int p, v; double b, l; juce::ValueTree properties; };   // properties: a copy of the note
    std::vector<CopiedNote> copiedNotes;

    /** A mouse cursor drawn from one of our icons: white with a dark outline. */
    juce::MouseCursor iconCursor (theme::Icon icon, float hotX, float hotY)
    {
        constexpr int size = 22;
        juce::Image image (juce::Image::ARGB, size, size, true);
        juce::Graphics g (image);
        const auto area = juce::Rectangle<float> (1.0f, 1.0f, size - 2.0f, size - 2.0f);
        theme::drawIcon (g, icon, area, juce::Colour (0xff120f19), 3.4f);
        theme::drawIcon (g, icon, area, juce::Colours::white, 1.6f);
        const float scale = (size - 2.0f) / 16.0f;
        return juce::MouseCursor (image, juce::roundToInt (1.0f + hotX * scale), juce::roundToInt (1.0f + hotY * scale));
    }


    /** The snap choices (beats; 0 = none), like FL's. */
    struct SnapChoice { double beats; const char* name; };
    const SnapChoice snapChoices[] =
    {
        { EditorState::snapBar, NEEDS_TRANS ("Bar") },
        { 1.0,               NEEDS_TRANS ("Beat") },
        { 0.5,               NEEDS_TRANS ("1/2 beat") },
        { stepBeats,         NEEDS_TRANS ("Step") },
        { stepBeats / 2.0,   NEEDS_TRANS ("1/2 step") },
        { stepBeats / 3.0,   NEEDS_TRANS ("1/3 step") },
        { stepBeats / 4.0,   NEEDS_TRANS ("1/4 step") },
        { 0.0,               NEEDS_TRANS ("None") },
    };

    juce::String snapName (double beats)
    {
        for (auto& c : snapChoices)
            if (std::abs (c.beats - beats) < 1.0e-9)
                return TRANS (c.name);

        return TRANS ("Step");
    }
}

//==============================================================================
class PianoRoll::Grid  : public juce::Component,
                        public juce::DragAndDropTarget,
                        public juce::FileDragAndDropTarget
{
public:
    explicit Grid (EditorState& s) : state (s)
    {
        setSize (600, (highestPitch - lowestPitch + 1) * rowHeight);
        setWantsKeyboardFocus (true);
    }

    // The tools' mouse pointers (made here, not as statics: they must go before the window system does).
    const juce::MouseCursor pencilCursor = iconCursor (theme::Icon::pencil, 2.5f, 13.5f);
    const juce::MouseCursor sliceCursor = iconCursor (theme::Icon::slice, 8.0f, 6.5f);

    double pixelsPerBeat = 40.0;                            // set by the piano roll (zoom)
    std::function<void (float factor, float x)> onZoom;     // Ctrl+wheel at x (in this component)
    std::function<void()> onToolKey;                        // P / E / C changed the tool
    std::function<void()> onPreviewChanged;                 // (the keyboard shows the key)
    std::function<void (int)> onStepChannel;                // Alt+Up / Alt+Down: another instrument
    std::function<void (const juce::String&)> onGhostChosen; // a ghost note was double-clicked: edit its instrument

    int pitchAtY (float y) const       { return juce::jlimit (lowestPitch, highestPitch, highestPitch - (int) std::floor (y / rowHeight)); }
    float yForPitch (int pitch) const   { return (float) ((highestPitch - pitch) * rowHeight); }
    double beatAtX (float x) const     { return juce::jmax (0.0, x / pixelsPerBeat); }
    float xForBeat (double beat) const  { return (float) (beat * pixelsPerBeat); }

    /** The snap grid in beats, 0 = off. Alt held = off, like FL. */
    double gridBeats() const
    {
        return juce::ModifierKeys::currentModifiers.isAltDown() ? 0.0 : juce::jmax (0.0, state.resolveSnap (state.pianoSnapBeats));
    }

    double snapDown (double beat) const
    {
        const double g = gridBeats();
        return g > 0.0 ? std::floor (beat / g + 1.0e-6) * g : beat;
    }

    double snapNearest (double beat) const
    {
        const double g = gridBeats();
        return g > 0.0 ? std::round (beat / g) * g : beat;
    }

    /** The grid step used by quantize and the arrow keys (a step when snapping is off). */
    double moveStep() const
    {
        const double g = gridBeats();
        return g > 0.0 ? g : stepBeats;
    }

    static constexpr double shortestNote = stepBeats / 4.0;   // (song::addNote's minimum)

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

    //==============================================================================
    // Notes and the selection (sync IDs: this PC only; dropped when the pattern or channel changes)
    juce::StringArray selected;
    juce::String contextKey;

    juce::Array<juce::ValueTree> currentNotes() const
    {
        auto pattern = state.getPattern();
        auto* channel = getNoteChannel();

        if (channel == nullptr || ! pattern.isValid())
            return {};

        return getNotes (pattern, getChannelID (*channel));
    }

    bool isSelected (const juce::ValueTree& n) const   { return selected.contains (n[ids::sid].toString()); }

    juce::Array<juce::ValueTree> selectedNotes() const
    {
        juce::Array<juce::ValueTree> result;

        if (! selected.isEmpty())
            for (auto& n : currentNotes())
                if (isSelected (n))
                    result.add (n);

        return result;
    }

    /** What most keys work on: the selection, or every note when nothing is selected (like FL). */
    juce::Array<juce::ValueTree> targetNotes() const
    {
        auto s = selectedNotes();
        return s.isEmpty() ? currentNotes() : s;
    }

    /** Called when the view changes: a different pattern or channel drops the selection. */
    void checkContext()
    {
        auto* channel = getNoteChannel();
        const auto key = state.getPattern()[ids::sid].toString() + "|" + (channel != nullptr ? getChannelID (*channel) : juce::String());

        if (key != contextKey)
        {
            contextKey = key;
            selected.clear();
            action = Action::none;
            held.clear();
        }
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        auto pattern = state.getPattern();
        auto* channel = getNoteChannel();
        const double length = getLengthBeats (pattern);
        const auto clip = g.getClipBounds();

        g.fillAll (theme::colours::lane2);

        // Rows
        for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
        {
            const float y = yForPitch (pitch);

            if (y > clip.getBottom() || y + rowHeight < clip.getY())
                continue;

            const bool black = isBlackKey (pitch);
            const auto row = juce::Rectangle<float> ((float) clip.getX(), y, (float) clip.getWidth(), (float) rowHeight);

            if (state.scaleSnap)
            {
                // Scale rows light, the others dark; the key note's rows tinted.
                const bool inScale = scales::contains (state.scaleKey, state.scaleType, pitch);
                g.setColour (inScale ? theme::colours::lane1.brighter (0.16f) : theme::colours::bg.darker (0.35f));
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

            if (pitch % 12 == 0)
            {
                g.setColour (theme::colours::gridBar);
                g.drawHorizontalLine ((int) (y + rowHeight) - 1, (float) clip.getX(), (float) clip.getRight());
            }
        }

        // Vertical lines: bars, beats, steps, and the snap grid when it is finer and there is room.
        const double firstBeat = beatAtX ((float) clip.getX()), lastBeat = juce::jmin (length, beatAtX ((float) clip.getRight()) + 0.25);
        const double snap = state.resolveSnap (state.pianoSnapBeats);
        const int barSteps = getStepsPerBar (state.getPattern());

        if (snap > 0.0 && snap < stepBeats - 1.0e-9 && snap * pixelsPerBeat >= 6.0)
        {
            g.setColour (theme::colours::grid.withAlpha (0.45f));

            for (double b = std::floor (firstBeat / snap) * snap; b <= lastBeat; b += snap)
                g.drawVerticalLine ((int) xForBeat (b), 0.0f, (float) getHeight());
        }

        const bool showSteps = stepBeats * pixelsPerBeat >= 4.0;

        for (int s = (int) std::floor (firstBeat / stepBeats); s * stepBeats <= lastBeat + 1.0e-9; ++s)
        {
            if (s % 4 != 0 && ! showSteps)
                continue;

            g.setColour (s % barSteps == 0 ? theme::colours::gridBar.brighter (0.25f) : s % 4 == 0 ? theme::colours::gridBar : theme::colours::grid);
            g.drawVerticalLine ((int) xForBeat (s * stepBeats), 0.0f, (float) getHeight());
        }

        // Start marker (where Play starts)
        g.setColour (theme::colours::green.withAlpha (0.55f));
        g.drawVerticalLine ((int) xForBeat (state.getStartBeat()), 0.0f, (float) getHeight());

        if (state.ghostNotes && pattern.isValid())
            paintGhostNotes (g, channel, length);

        if (channel != nullptr && pattern.isValid())
            paintNotes (g, *channel, length);

        // The selection box
        if (action == Action::marquee)
        {
            g.setColour (theme::colours::accent.withAlpha (0.14f));
            g.fillRect (marqueeArea);
            g.setColour (theme::colours::accent);
            g.drawRect (marqueeArea, 1.0f);
        }

        // The slice line
        if (action == Action::slice)
        {
            const float x = xForBeat (sliceBeat);
            const float y1 = yForPitch (juce::jmax (slicePitch1, slicePitch2));
            const float y2 = yForPitch (juce::jmin (slicePitch1, slicePitch2)) + rowHeight;
            g.setColour (theme::colours::accent);
            g.fillRect (juce::Rectangle<float> (x - 1.0f, y1 - 4.0f, 2.0f, y2 - y1 + 8.0f));
        }

        // Playhead
        const double playBeat = state.getPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (theme::colours::text);
            g.fillRect (juce::Rectangle<float> (xForBeat (playBeat) - 1.0f, 0.0f, 2.0f, (float) getHeight()));
        }
    }

    /** FL's ghost notes: every other instrument's notes in this pattern, faded, in its colour. */
    void paintGhostNotes (juce::Graphics& g, te::AudioTrack* current, double length)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const auto clip = g.getClipBounds().toFloat();
        const auto channels = getChannels (*edit);

        for (int i = 0; i < channels.size(); ++i)
        {
            if (channels[i] == current || isAudioChannel (*channels[i]))
                continue;

            const auto colour = EditorState::channelColour (i);

            for (auto& n : getNotes (state.getPattern(), getChannelID (*channels[i])))
            {
                auto r = noteBounds (n).reduced (0.5f, 2.0f);

                if (r.getX() >= xForBeat (length) || ! r.intersects (clip))
                    continue;

                g.setColour (colour.withAlpha (0.14f));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (colour.withAlpha (0.38f));
                g.drawRoundedRectangle (r, 3.0f, 1.0f);
            }
        }
    }

    /** The instrument of the ghost note under a point ("" = none). */
    juce::String ghostChannelAt (juce::Point<float> p) const
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || ! state.ghostNotes)
            return {};

        auto* current = getNoteChannel();

        for (auto* t : getChannels (*edit))
            if (t != current && ! isAudioChannel (*t))
                for (auto& n : getNotes (state.getPattern(), getChannelID (*t)))
                    if (noteBounds (n).contains (p))
                        return getChannelID (*t);

        return {};
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // On a ghost note: edit that instrument, like FL.
        const auto other = ghostChannelAt (e.position);
        auto* edit = state.getEdit();

        if (other.isEmpty() || onGhostChosen == nullptr || edit == nullptr)
            return;

        // One of your own notes is there (not the one the first click just drew): it wins.
        for (auto& n : currentNotes())
            if (noteBounds (n).contains (e.position) && n[ids::sid].toString() != justAddedSid)
                return;

        // The pencil's first click drew a note there: take it back.
        for (auto& n : currentNotes())
            if (n[ids::sid].toString() == justAddedSid)
                removeNote (*edit, n);

        justAddedSid = {};
        selected.clear();
        onGhostChosen (other);
    }

    juce::String justAddedSid;   // the note the pencil's last click drew (a double-click on a ghost takes it back)

    void paintNotes (juce::Graphics& g, te::AudioTrack& channel, double length)
    {
        const auto colour = EditorState::channelColour (getChannels (channel.edit).indexOf (&channel));
        const auto clip = g.getClipBounds().toFloat();
        const auto font = theme::mono (9.0f, true);

        for (auto& n : getNotes (state.getPattern(), getChannelID (channel)))
        {
            auto r = noteBounds (n);

            if (r.getX() >= xForBeat (length) || ! r.intersects (clip))
                continue;

            const bool sel = isSelected (n);
            const auto body = r.reduced (0.5f, 1.0f);
            g.setColour (sel ? colour.brighter (0.45f) : colour);
            g.fillRoundedRectangle (body, 3.0f);
            g.setColour (sel ? juce::Colours::white : colour.darker (0.7f));
            g.drawRoundedRectangle (body, 3.0f, sel ? 1.5f : 1.0f);

            // The note's name when it fits (C5 = MIDI 60, as on the keyboard).
            if (body.getWidth() >= 26.0f)
            {
                const int p = n[ids::p];
                g.setColour (juce::Colour (0xff120f19).withAlpha (0.75f));
                g.setFont (font);
                g.drawText (juce::String (noteNames[p % 12]) + juce::String (p / 12), body.reduced (4.0f, 0.0f),
                            juce::Justification::centredLeft, false);
            }
        }
    }

    juce::Rectangle<float> noteBounds (const juce::ValueTree& n) const
    {
        const double b = n[ids::b], l = n[ids::l];
        const float x1 = xForBeat (b), x2 = xForBeat (b + l);
        return { x1, yForPitch ((int) n[ids::p]), juce::jmax (4.0f, x2 - x1), (float) rowHeight };
    }

    /** How close to a note's right end its length handle is. */
    static float edgeWidth (juce::Rectangle<float> r)  { return juce::jmin (6.0f, r.getWidth() * 0.34f); }

    juce::ValueTree noteAt (juce::Point<float> p) const
    {
        auto notes = currentNotes();

        for (int i = notes.size(); --i >= 0;)
            if (noteBounds (notes[i]).contains (p))
                return notes[i];

        return {};
    }

    //==============================================================================
    // Note preview (local only, like FL): hear the note while you place or drag it, but only
    // while the song is stopped (while it plays it would only confuse). The keyboard always plays.
    bool isPlaying() const
    {
        auto* edit = state.getEdit();
        return edit != nullptr && edit->getTransport().isPlaying();
    }

    void startPreview (int pitch, bool evenWhilePlaying = false)
    {
        if (pitch == previewPitch || (! evenWhilePlaying && isPlaying()))
            return;

        if (auto* channel = getNoteChannel())
        {
            channel->playGuideNote (pitch, te::MidiChannel (1), 100, true, true, false);
            previewPitch = pitch;

            if (onPreviewChanged)
                onPreviewChanged();
        }
    }

    void stopPreview()
    {
        if (previewPitch >= 0)
            if (auto* channel = getNoteChannel())
                channel->turnOffGuideNotes();

        previewPitch = -1;

        if (onPreviewChanged)
            onPreviewChanged();
    }

    int previewPitch = -1;

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (state.pianoTool == EditorState::PianoTool::slice)
        {
            setMouseCursor (sliceCursor);
            return;
        }

        auto n = noteAt (e.position);

        if (n.isValid())
        {
            const auto r = noteBounds (n);
            setMouseCursor (e.position.x > r.getRight() - edgeWidth (r) ? juce::MouseCursor::LeftRightResizeCursor
                                                                       : juce::MouseCursor::DraggingHandCursor);
        }
        else
        {
            setMouseCursor (state.pianoTool == EditorState::PianoTool::pencil && ! e.mods.isCommandDown()
                              ? pencilCursor : juce::MouseCursor (juce::MouseCursor::NormalCursor));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        auto* edit = state.getEdit();
        auto pattern = state.getPattern();
        auto* channel = getNoteChannel();
        action = Action::none;
        held.clear();
        copied = false;

        if (e.getNumberOfClicks() < 2)
            justAddedSid = {};   // (kept for the second click of a double-click, see mouseDoubleClick)

        if (edit == nullptr || channel == nullptr || ! pattern.isValid())
            return;

        checkContext();
        state.beginAction();
        const auto tool = state.pianoTool;
        auto hit = noteAt (e.position);

        // Right button: delete what it touches (keep it held and sweep to delete more), like FL.
        if (e.mods.isPopupMenu())
        {
            action = Action::erase;
            eraseAt (e.position);
            return;
        }

        if (tool == EditorState::PianoTool::slice)
        {
            action = Action::slice;
            sliceBeat = snapNearest (beatAtX (e.position.x));
            slicePitch1 = slicePitch2 = pitchAtY (e.position.y);
            repaint();
            return;
        }

        const bool selectTool = tool == EditorState::PianoTool::select;

        // A box to select notes: Select tool on empty space, or Ctrl+drag with the pencil.
        if (! hit.isValid() && (selectTool || e.mods.isCommandDown()))
        {
            action = Action::marquee;
            const bool adding = e.mods.isShiftDown() || (selectTool && e.mods.isCommandDown());
            marqueeBase = adding ? selected : juce::StringArray();
            marqueeArea = { e.position, e.position };
            selected = marqueeBase;
            repaint();
            return;
        }

        if (hit.isValid())
        {
            const auto sid = hit[ids::sid].toString();

            if (selectTool && e.mods.isCommandDown())   // Ctrl+click: add or remove one note
            {
                if (selected.contains (sid))
                    selected.removeString (sid);
                else
                    selected.add (sid);

                repaint();
                return;
            }

            if (! selected.contains (sid))
            {
                // A note outside the selection: the Select tool selects it; the pencil just takes it.
                selected.clear();

                if (selectTool)
                    selected.add (sid);
            }

            const auto r = noteBounds (hit);
            action = e.position.x > r.getRight() - edgeWidth (r) ? Action::resize : Action::move;
            lastLength = hit[ids::l];
            lastTouched = hit.createCopy();   // new notes copy its velocity, pan... (like FL)
            holdNotes (selected.contains (sid) ? selectedNotes() : juce::Array<juce::ValueTree> { hit }, hit, e.position);
            copyOnDrag = e.mods.isShiftDown() && action == Action::move;
            startPreview (anchorP);
        }
        else
        {
            // Pencil on empty space: a new note, as long as the last one you touched; keep
            // the button down to move it (like FL).
            const double length = getLengthBeats (pattern);
            const double beat = snapDown (beatAtX (e.position.x));

            if (beat >= length - 1.0e-6)
                return;

            const int pitch = snapPitch (pitchAtY (e.position.y));
            selected.clear();
            auto n = addNote (*edit, pattern, getChannelID (*channel), pitch, beat,
                              juce::jlimit (shortestNote, juce::jmax (shortestNote, length - beat), lastLength),
                              getNoteProperty (lastTouched, NoteProperty::velocity));
            copyNoteProperties (*edit, lastTouched, n);
            justAddedSid = n[ids::sid].toString();
            action = Action::move;
            holdNotes ({ n }, n, e.position);
            copyOnDrag = false;
            startPreview (pitch);
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        switch (action)
        {
            case Action::erase:
                eraseAt (e.position);
                break;

            case Action::slice:
                sliceBeat = snapNearest (beatAtX (e.position.x));
                slicePitch2 = pitchAtY (e.position.y);
                repaint();
                break;

            case Action::marquee:
            {
                marqueeArea = juce::Rectangle<float> (e.mouseDownPosition, e.position);
                selected = marqueeBase;

                for (auto& n : currentNotes())
                    if (noteBounds (n).intersects (marqueeArea))
                        selected.addIfNotAlreadyThere (n[ids::sid].toString());

                repaint();
                break;
            }

            case Action::move:     if (e.mouseWasDraggedSinceMouseDown()) dragMove (e); break;
            case Action::resize:   if (e.mouseWasDraggedSinceMouseDown()) dragResize (e); break;
            case Action::none:
            default:               break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (action == Action::slice)
            sliceNotes (sliceBeat, juce::jmin (slicePitch1, slicePitch2), juce::jmax (slicePitch1, slicePitch2));

        action = Action::none;
        held.clear();
        stopPreview();
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        // Ctrl+wheel: zoom in time around the mouse (FL). Otherwise the view scrolls.
        if (e.mods.isCommandDown() && onZoom != nullptr)
        {
            const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;

            if (d != 0.0f)
                onZoom (d > 0.0f ? 1.25f : 0.8f, e.position.x);

            return;
        }

        juce::Component::mouseWheelMove (e, w);
    }

    //==============================================================================
    // Dragging notes
    enum class Action { none, move, resize, marquee, erase, slice };
    Action action = Action::none;

    struct Held { juce::ValueTree note; double b, l; int p; };
    std::vector<Held> held;            // the notes being moved or resized, as they were
    double anchorB = 0.0, anchorL = 0.0, grabBeat = 0.0, lastLength = stepBeats;
    juce::ValueTree lastTouched { ids::PNOTE };   // the last note you clicked (new notes copy its properties)
    int anchorP = 60;
    bool copyOnDrag = false, copied = false;
    juce::Rectangle<float> marqueeArea;
    juce::StringArray marqueeBase;
    double sliceBeat = 0.0;
    int slicePitch1 = 60, slicePitch2 = 60;

    void holdNotes (const juce::Array<juce::ValueTree>& notes, const juce::ValueTree& anchor, juce::Point<float> mouse)
    {
        held.clear();

        for (auto& n : notes)
            held.push_back ({ n, (double) n[ids::b], (double) n[ids::l], (int) n[ids::p] });

        anchorB = anchor[ids::b];
        anchorL = anchor[ids::l];
        anchorP = anchor[ids::p];
        grabBeat = beatAtX (mouse.x);
    }

    void dragMove (const juce::MouseEvent& e)
    {
        auto* edit = state.getEdit();
        auto pattern = state.getPattern();
        auto* channel = getNoteChannel();

        if (edit == nullptr || channel == nullptr || held.empty())
            return;

        auto* um = &edit->getUndoManager();
        const double length = getLengthBeats (pattern);

        // Shift+drag: copies move, the originals stay (made on the first real move).
        if (copyOnDrag && ! copied)
        {
            copied = true;
            selected.clear();

            for (auto& h : held)
            {
                auto original = h.note;
                h.note = addNote (*edit, pattern, getChannelID (*channel), h.p, h.b, h.l, (int) h.note.getProperty (ids::v, 100));
                copyNoteProperties (*edit, original, h.note);
                selected.add (h.note[ids::sid].toString());
            }
        }

        // Time: the grabbed note lands on the grid; the others keep their distance to it.
        double minB = length, maxB = 0.0;

        for (auto& h : held)
        {
            minB = juce::jmin (minB, h.b);
            maxB = juce::jmax (maxB, h.b);
        }

        const double newAnchor = snapNearest (anchorB + beatAtX (e.position.x) - grabBeat);
        const double deltaB = juce::jlimit (-minB, juce::jmax (-minB, length - shortestNote - maxB), newAnchor - anchorB);

        // Pitch: rows moved (the scale helper puts the grabbed note on the scale; a chord keeps its shape).
        const int rows = pitchAtY (e.position.y) - pitchAtY (e.mouseDownPosition.y);
        int newP = juce::jlimit (lowestPitch, highestPitch, anchorP + rows);

        if (rows != 0)
            newP = snapPitch (newP, rows > 0);

        int lowP = highestPitch, highP = lowestPitch;

        for (auto& h : held)
        {
            lowP = juce::jmin (lowP, h.p);
            highP = juce::jmax (highP, h.p);
        }

        const int deltaP = juce::jlimit (lowestPitch - lowP, highestPitch - highP, newP - anchorP);

        for (auto& h : held)
        {
            const double b = h.b + deltaB;
            const int p = h.p + deltaP;

            if (std::abs (b - (double) h.note[ids::b]) > 1.0e-9)
                h.note.setProperty (ids::b, b, um);

            if (p != (int) h.note[ids::p])
                h.note.setProperty (ids::p, p, um);
        }

        startPreview (anchorP + deltaP);
        repaint();
    }

    void dragResize (const juce::MouseEvent& e)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || held.empty())
            return;

        auto* um = &edit->getUndoManager();
        const double length = getLengthBeats (state.getPattern());
        const double g = gridBeats();
        const double newEnd = snapNearest (beatAtX (e.position.x));
        const double delta = newEnd - (anchorB + anchorL);

        for (auto& h : held)
        {
            // At least one grid step (or the note's own length, if it was shorter), inside the pattern.
            const double shortest = juce::jmin (h.l, juce::jmax (shortestNote, g));
            const double l = juce::jlimit (shortest, juce::jmax (shortest, length - h.b), h.l + delta);

            if (std::abs (l - (double) h.note[ids::l]) > 1.0e-9)
                h.note.setProperty (ids::l, l, um);
        }

        lastLength = held.front().note[ids::l];

        for (auto& h : held)
            if (h.b == anchorB && h.p == anchorP)
                lastLength = h.note[ids::l];

        repaint();
    }

    void eraseAt (juce::Point<float> p)
    {
        auto* edit = state.getEdit();
        auto hit = noteAt (p);

        if (edit != nullptr && hit.isValid())
        {
            selected.removeString (hit[ids::sid].toString());
            removeNote (*edit, hit);
            repaint();
        }
    }

    /** Cuts every note between two pitches that goes across a beat into two notes. */
    void sliceNotes (double at, int lowPitch, int highPitch)
    {
        auto* edit = state.getEdit();
        auto pattern = state.getPattern();
        auto* channel = getNoteChannel();

        if (edit == nullptr || channel == nullptr)
            return;

        auto* um = &edit->getUndoManager();

        for (auto& n : currentNotes())
        {
            const int p = n[ids::p];
            const double b = n[ids::b], l = n[ids::l], end = b + l;

            if (p < lowPitch || p > highPitch || at - b < shortestNote - 1.0e-9 || end - at < shortestNote - 1.0e-9)
                continue;

            n.setProperty (ids::l, at - b, um);
            auto second = addNote (*edit, pattern, getChannelID (*channel), p, at, end - at, (int) n.getProperty (ids::v, 100));
            copyNoteProperties (*edit, n, second);

            if (isSelected (n))
                selected.add (second[ids::sid].toString());
        }
    }

    //==============================================================================
    // Keys
    bool keyPressed (const juce::KeyPress& key) override
    {
        const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
        auto is = [&key] (int code, int mods = 0) { return key == juce::KeyPress (code, mods, 0); };

        if (is ('p'))   return setTool (EditorState::PianoTool::pencil);
        if (is ('e'))   return setTool (EditorState::PianoTool::select);
        if (is ('c'))   return setTool (EditorState::PianoTool::slice);

        const int alt = juce::ModifierKeys::altModifier;

        if (is (juce::KeyPress::upKey, alt) || is (juce::KeyPress::downKey, alt))
        {
            if (onStepChannel)
                onStepChannel (is (juce::KeyPress::upKey, alt) ? -1 : 1);

            return true;
        }

        if (is ('a', cmd))   { selected.clear(); for (auto& n : currentNotes()) selected.add (n[ids::sid].toString()); repaint(); return true; }
        if (is ('d', cmd))   { selected.clear(); repaint(); return true; }
        if (is ('c', cmd))   return copyNotes (false);
        if (is ('x', cmd))   return copyNotes (true);
        if (is ('v', cmd))   return pasteNotes();
        if (is ('b', cmd))   return duplicateNotes();
        if (is ('l', cmd))   return legato();
        if (is ('q', cmd))   return quantize();

        if (is (juce::KeyPress::deleteKey) || is (juce::KeyPress::backspaceKey))
            return deleteSelected();

        if (is (juce::KeyPress::upKey))                                 return transpose (1);
        if (is (juce::KeyPress::downKey))                               return transpose (-1);
        if (is (juce::KeyPress::upKey, cmd) || is (juce::KeyPress::upKey, shift))       return transpose (12);
        if (is (juce::KeyPress::downKey, cmd) || is (juce::KeyPress::downKey, shift))   return transpose (-12);
        if (is (juce::KeyPress::leftKey))                               return shiftTime (-1);
        if (is (juce::KeyPress::rightKey))                              return shiftTime (1);

        return false;   // (Space, Ctrl+Z, Esc... go on to the main window)
    }

    bool setTool (EditorState::PianoTool t)
    {
        state.pianoTool = t;

        if (onToolKey)
            onToolKey();

        return true;
    }

    /** Starts an undo step and gives what an edit needs; false if there is nothing to edit. */
    bool begin (te::Edit*& edit, juce::ValueTree& pattern, te::AudioTrack*& channel)
    {
        edit = state.getEdit();
        pattern = state.getPattern();
        channel = getNoteChannel();

        if (edit == nullptr || channel == nullptr || ! pattern.isValid())
            return false;

        checkContext();
        state.beginAction();
        return true;
    }

    bool copyNotes (bool cut)
    {
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (! begin (edit, pattern, channel))
            return true;

        auto notes = targetNotes();

        if (notes.isEmpty())
            return true;

        copiedNotes.clear();

        for (auto& n : notes)
            copiedNotes.push_back ({ (int) n[ids::p], (int) n.getProperty (ids::v, 100), (double) n[ids::b], (double) n[ids::l], n.createCopy() });

        if (cut)
        {
            for (auto& n : notes)
                removeNote (*edit, n);

            selected.clear();
        }

        repaint();
        return true;
    }

    bool pasteNotes()
    {
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (copiedNotes.empty() || ! begin (edit, pattern, channel))
            return true;

        const double length = getLengthBeats (pattern);
        selected.clear();

        for (auto& c : copiedNotes)
            if (c.b < length - 1.0e-6)
            {
                auto note = addNote (*edit, pattern, getChannelID (*channel), c.p, c.b, juce::jmin (c.l, length - c.b), c.v);
                copyNoteProperties (*edit, c.properties, note);
                selected.add (note[ids::sid].toString());
            }

        repaint();
        return true;
    }

    /** Ctrl+B: a copy of the notes right after them (FL). The jump is their span rounded up
        to a step (under a beat), a beat (under a bar) or a bar; the pattern grows if needed. */
    bool duplicateNotes()
    {
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (! begin (edit, pattern, channel))
            return true;

        auto notes = targetNotes();

        if (notes.isEmpty())
            return true;

        double start = 1.0e9, end = 0.0;

        for (auto& n : notes)
        {
            start = juce::jmin (start, (double) n[ids::b]);
            end = juce::jmax (end, (double) n[ids::b] + (double) n[ids::l]);
        }

        const double span = end - start;
        const double bar = getBeatsPerBar (pattern);
        const double unit = span < 1.0 - 1.0e-9 ? stepBeats : span < bar - 1.0e-9 ? 1.0 : bar;
        const double jump = juce::jmax (unit, std::ceil (end / unit - 1.0e-6) * unit - std::floor (start / unit + 1.0e-6) * unit);
        const double newEnd = end + jump;

        if (newEnd > getLengthBeats (pattern) + 1.0e-6)
            setBars (*edit, pattern, juce::jmin (8, (int) std::ceil (newEnd / bar - 1.0e-6)));

        const double length = getLengthBeats (pattern);
        int left = 0;
        selected.clear();

        for (auto& n : notes)
        {
            const double b = (double) n[ids::b] + jump;

            if (b >= length - 1.0e-6)
            {
                ++left;
                continue;
            }

            auto copy = addNote (*edit, pattern, getChannelID (*channel), n[ids::p], b,
                                 juce::jmin ((double) n[ids::l], length - b), (int) n.getProperty (ids::v, 100));
            copyNoteProperties (*edit, n, copy);
            selected.add (copy[ids::sid].toString());
        }

        if (left > 0)
            state.session.logMessage (TRANS ("A pattern is at most 8 bars: {N} note(s) did not fit")
                                        .replace ("{N}", juce::String (left)));

        repaint();
        return true;
    }

    /** Ctrl+L: each note lasts until the next note starts (FL's quick legato); the last ones stay. */
    bool legato()
    {
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (! begin (edit, pattern, channel))
            return true;

        auto notes = targetNotes();
        std::vector<double> starts;

        for (auto& n : notes)
            starts.push_back (n[ids::b]);

        std::sort (starts.begin(), starts.end());

        for (auto& n : notes)
        {
            const double b = n[ids::b];
            auto next = std::upper_bound (starts.begin(), starts.end(), b + 1.0e-6);

            if (next != starts.end() && std::abs ((*next - b) - (double) n[ids::l]) > 1.0e-9)
                n.setProperty (ids::l, *next - b, &edit->getUndoManager());
        }

        repaint();
        return true;
    }

    /** Ctrl+Q: note starts onto the nearest grid line (lengths stay). */
    bool quantize()
    {
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (! begin (edit, pattern, channel))
            return true;

        const double g = moveStep(), length = getLengthBeats (pattern);

        for (auto& n : targetNotes())
        {
            const double b = n[ids::b];
            double q = std::round (b / g) * g;

            if (q >= length - 1.0e-6)
                q = std::floor (b / g) * g;

            if (std::abs (q - b) > 1.0e-9)
                n.setProperty (ids::b, q, &edit->getUndoManager());
        }

        repaint();
        return true;
    }

    bool deleteSelected()
    {
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (! begin (edit, pattern, channel))
            return true;

        for (auto& n : selectedNotes())
            removeNote (*edit, n);

        selected.clear();
        repaint();
        return true;
    }

    /** Up / down: the selected notes (nothing selected: the key goes on, e.g. to scroll). */
    bool transpose (int semitones)
    {
        auto notes = selectedNotes();
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (notes.isEmpty() || ! begin (edit, pattern, channel))
            return false;

        int lowP = highestPitch, highP = lowestPitch;

        for (auto& n : notes)
        {
            lowP = juce::jmin (lowP, (int) n[ids::p]);
            highP = juce::jmax (highP, (int) n[ids::p]);
        }

        const int d = juce::jlimit (lowestPitch - lowP, highestPitch - highP, semitones);

        if (d != 0)
            for (auto& n : notes)
                n.setProperty (ids::p, (int) n[ids::p] + d, &edit->getUndoManager());

        repaint();
        return true;
    }

    /** Left / right: the selected notes one grid step (a step when snap is off). */
    bool shiftTime (int direction)
    {
        auto notes = selectedNotes();
        te::Edit* edit; juce::ValueTree pattern; te::AudioTrack* channel;

        if (notes.isEmpty() || ! begin (edit, pattern, channel))
            return false;

        double minB = 1.0e9, maxB = 0.0;

        for (auto& n : notes)
        {
            minB = juce::jmin (minB, (double) n[ids::b]);
            maxB = juce::jmax (maxB, (double) n[ids::b]);
        }

        const double length = getLengthBeats (pattern);
        const double d = juce::jlimit (-minB, juce::jmax (-minB, length - shortestNote - maxB), direction * moveStep());

        if (std::abs (d) > 1.0e-9)
            for (auto& n : notes)
                n.setProperty (ids::b, (double) n[ids::b] + d, &edit->getUndoManager());

        repaint();
        return true;
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
};

//==============================================================================
/** The keyboard on the left: scrolls up and down with the notes; click to play a key. */
class PianoRoll::Keys  : public juce::Component
{
public:
    Keys (EditorState& s, Grid& g) : state (s), grid (g)
    {
        setSize (keyboardWidth, grid.getHeight());
    }

    std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colours::lane2);

        for (int pitch = lowestPitch; pitch <= highestPitch; ++pitch)
        {
            const float y = grid.yForPitch (pitch);
            const bool black = Grid::isBlackKey (pitch);

            // Light lilac-white and deep purple-black; the key you are hearing in the accent.
            g.setColour (pitch == grid.previewPitch ? theme::colours::accent
                                                    : black ? juce::Colour (0xff120f19) : juce::Colour (0xffe6e0f4));
            g.fillRect (0.0f, y, (float) keyboardWidth - 2.0f, (float) rowHeight - 1.0f);

            if (state.scaleSnap && scales::contains (state.scaleKey, state.scaleType, pitch))
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
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override     { grid.startPreview (grid.pitchAtY (e.position.y), true); }
    void mouseDrag (const juce::MouseEvent& e) override     { grid.startPreview (grid.pitchAtY (e.position.y), true); }   // glide
    void mouseUp (const juce::MouseEvent&) override         { grid.stopPreview(); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (onWheel)
            onWheel (e, w);
    }

private:
    EditorState& state;
    Grid& grid;
};

//==============================================================================
/** Time ruler above the piano roll: click or drag to set where Play starts.
    While playing, clicking jumps there (for everyone). Ctrl+wheel zooms. */
class PianoRoll::Ruler  : public juce::Component
{
public:
    Ruler (EditorState& s, Grid& g) : state (s), grid (g) {}

    std::function<int()> getViewX;
    std::function<void (float factor, float gridX)> onZoom;
    std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;

    float viewX() const                 { return getViewX != nullptr ? (float) getViewX() : 0.0f; }
    float xForBeat (double beat) const  { return keyboardWidth + grid.xForBeat (beat) - viewX(); }
    float gridX (float x) const         { return x - keyboardWidth + viewX(); }

    void paint (juce::Graphics& g) override
    {
        auto pattern = state.getPattern();
        const int numSteps = getNumSteps (pattern);
        const int barSteps = getStepsPerBar (pattern);

        g.fillAll (theme::colours::panel);
        g.setColour (theme::colours::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (keyboardWidth, 0, getWidth() - keyboardWidth, getHeight());

            // Beat numbers only when there is room for them (bars always).
            const bool beatLabels = grid.pixelsPerBeat >= 34.0;

            for (int s = 0; s <= numSteps; s += 4)
            {
                const float x = xForBeat (s * stepBeats);

                if (x < keyboardWidth - 2.0f || x > (float) getWidth() + 2.0f)
                    continue;

                g.setColour (s % barSteps == 0 ? theme::colours::muted : theme::colours::gridBar);
                g.drawVerticalLine ((int) x, s % barSteps == 0 ? 2.0f : 10.0f, (float) getHeight());

                if (s < numSteps && (s % barSteps == 0 || beatLabels))
                {
                    g.setFont (theme::mono (10.0f, s % barSteps == 0));
                    g.setColour (s % barSteps == 0 ? theme::colours::text : theme::colours::dim);
                    g.drawText (juce::String (s / barSteps + 1) + (s % barSteps == 0 ? juce::String() : "." + juce::String ((s % barSteps) / 4 + 1)),
                                (int) x + 3, 0, 40, getHeight(), juce::Justification::centredLeft);
                }
            }

            // Start marker
            const float mx = xForBeat (state.getStartBeat());
            juce::Path tri;
            tri.addTriangle (mx - 6.0f, 2.0f, mx + 6.0f, 2.0f, mx, (float) getHeight() - 2.0f);
            g.setColour (theme::colours::green);
            g.fillPath (tri);

            const double playBeat = state.getPlayheadBeat();

            if (playBeat >= 0.0)
            {
                g.setColour (theme::colours::text);
                g.fillRect (xForBeat (playBeat) - 1.0f, 0.0f, 2.0f, (float) getHeight());
            }
        }

        g.setColour (theme::colours::dim);
        g.setFont (theme::ui (10.0f));
        g.drawText (TRANS ("start"), 4, 0, keyboardWidth - 8, getHeight(), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override    { setStart (e.position.x, true); }
    void mouseDrag (const juce::MouseEvent& e) override    { setStart (e.position.x, false); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() && onZoom != nullptr)
        {
            const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;

            if (d != 0.0f)
                onZoom (d > 0.0f ? 1.25f : 0.8f, gridX (e.position.x));
        }
        else if (onWheel)
        {
            onWheel (e, w);
        }
    }

    void setStart (float x, bool seekIfPlaying)
    {
        state.startBeat = grid.snapDown (grid.beatAtX (gridX (juce::jmax ((float) keyboardWidth, x))));
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
/** FL's event editor, simplified: one bar per note of the instrument you edit, showing its
    velocity, release velocity, pan or fine pitch (click the name on the left to switch).
    Drag across the bars to draw values; with notes selected only those change. Right-drag
    puts them back to normal. Shared and undoable (one step per drag). */
class PianoRoll::PropertyLane  : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    PropertyLane (EditorState& s, Grid& g) : state (s), grid (g)
    {
        setTooltip (TRANS ("Each note's velocity, release, pan or fine pitch: drag across the bars to set them "
                           "(only the selected notes, if any), right-drag to reset. Click the name to switch. "
                           "Pan and fine pitch work on the built-in Sampler and drum sounds"));
    }

    std::function<int()> getViewX;
    std::function<void()> onPropertyChanged;

    float viewX() const                  { return getViewX != nullptr ? (float) getViewX() : 0.0f; }
    float xForBeat (double beat) const   { return keyboardWidth + grid.xForBeat (beat) - viewX(); }

    juce::Rectangle<float> valueArea() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft ((float) keyboardWidth).withTrimmedTop (8.0f).withTrimmedBottom (5.0f);
    }

    bool isCentred() const   { return state.laneProperty == NoteProperty::pan || state.laneProperty == NoteProperty::fine; }

    float yForValue (int value) const
    {
        const auto r = valueArea();
        const auto range = getNotePropertyRange (state.laneProperty);
        const float t = (float) (value - range.getStart()) / (float) juce::jmax (1, range.getLength());
        return r.getBottom() - t * r.getHeight();
    }

    int valueAtY (float y) const
    {
        const auto r = valueArea();
        const auto range = getNotePropertyRange (state.laneProperty);
        const float t = juce::jlimit (0.0f, 1.0f, (r.getBottom() - y) / juce::jmax (1.0f, r.getHeight()));
        return range.getStart() + juce::roundToInt (t * (float) range.getLength());
    }

    static juce::String nameOf (NoteProperty p)
    {
        switch (p)
        {
            case NoteProperty::velocity:  return TRANS ("Velocity");
            case NoteProperty::release:   return TRANS ("Release");
            case NoteProperty::pan:       return TRANS ("Pan");
            case NoteProperty::fine:      return TRANS ("Fine pitch");
        }

        return {};
    }

    static juce::String shortNameOf (NoteProperty p)
    {
        switch (p)
        {
            case NoteProperty::velocity:  return "VEL";
            case NoteProperty::release:   return "REL";
            case NoteProperty::pan:       return "PAN";
            case NoteProperty::fine:      return "FINE";
        }

        return {};
    }

    static juce::String valueText (NoteProperty p, int v)
    {
        if (p == NoteProperty::pan)
            return v == 0 ? TRANS ("centre") : juce::String (std::abs (v)) + "% " + (v < 0 ? TRANS ("left") : TRANS ("right"));

        if (p == NoteProperty::fine)
            return (v > 0 ? "+" : "") + juce::String (v) + " " + TRANS ("cents");

        return juce::String (v);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colours::well);
        g.setColour (theme::colours::line);
        g.fillRect (0, 0, getWidth(), 1);

        // The name (click to switch), on the left under the keyboard.
        auto label = getLocalBounds().removeFromLeft (keyboardWidth).reduced (4, 6);
        g.setColour (theme::colours::wellAlt);
        g.fillRoundedRectangle (label.toFloat(), 4.0f);
        g.setColour (theme::colours::text);
        g.setFont (theme::ui (10.0f, theme::Weight::bold));
        g.drawText (shortNameOf (state.laneProperty), label.withTrimmedBottom (label.getHeight() / 2), juce::Justification::centredBottom, false);
        g.setColour (theme::colours::muted);
        g.setFont (theme::ui (9.0f));
        g.drawText (juce::String::fromUTF8 ("\xe2\x96\xbe"), label.withTrimmedTop (label.getHeight() / 2), juce::Justification::centredTop, false);

        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (keyboardWidth, 0, getWidth() - keyboardWidth, getHeight());

        const auto area = valueArea();
        const auto pattern = state.getPattern();
        const int barSteps = getStepsPerBar (pattern);

        // Beat lines, like the notes above.
        for (int st = 0; st <= getNumSteps (pattern); st += 4)
        {
            g.setColour (st % barSteps == 0 ? theme::colours::gridBar : theme::colours::grid);
            g.drawVerticalLine ((int) xForBeat (st * stepBeats), area.getY(), area.getBottom());
        }

        // The middle (pan / fine) or the normal value (velocity 100, release 64).
        const int normal = getNotePropertyDefault (state.laneProperty);
        const float baseY = isCentred() ? yForValue (0) : area.getBottom();
        g.setColour (theme::colours::grid.brighter (0.2f));
        g.drawHorizontalLine ((int) yForValue (normal), area.getX(), area.getRight());

        auto* channel = grid.getNoteChannel();
        auto* edit = state.getEdit();

        if (channel == nullptr || edit == nullptr)
            return;

        const auto colour = EditorState::channelColour (getChannels (*edit).indexOf (channel));
        const bool anySelected = ! grid.selected.isEmpty();

        for (auto& n : grid.currentNotes())
        {
            const float x = xForBeat (n[ids::b]) + 1.5f;

            if (x < keyboardWidth - 4.0f || x > (float) getWidth() + 4.0f)
                continue;

            const int value = getNoteProperty (n, state.laneProperty);
            const float y = yForValue (value);
            const bool sel = grid.selected.contains (n[ids::sid].toString());
            const auto c = anySelected && ! sel ? colour.withAlpha (0.35f) : sel ? colour.brighter (0.45f) : colour;
            g.setColour (c);
            g.fillRect (juce::Rectangle<float> (x - 1.0f, juce::jmin (y, baseY), 2.0f, std::abs (baseY - y) + 1.0f));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y }));
        }

        // The value you are drawing, next to the mouse.
        if (shownValue.isNotEmpty())
        {
            juce::Font font (theme::ui (10.5f, theme::Weight::semibold));
            const float w = juce::GlyphArrangement::getStringWidth (font, shownValue) + 10.0f;
            auto tag = juce::Rectangle<float> (juce::jlimit ((float) keyboardWidth, (float) getWidth() - w, shownAt.x + 10.0f),
                                               juce::jlimit (2.0f, (float) getHeight() - 18.0f, shownAt.y - 8.0f), w, 16.0f);
            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.fillRoundedRectangle (tag, 4.0f);
            g.setColour (theme::colours::text);
            g.setFont (font);
            g.drawText (shownValue, tag, juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.position.x < (float) keyboardWidth)
        {
            showPropertyMenu();
            return;
        }

        state.beginAction();
        lastX = e.position.x;
        drawAt (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mouseDownPosition.x < (float) keyboardWidth)
            return;

        drawAt (e);
        lastX = e.position.x;
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        shownValue = {};
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (onWheel)
            onWheel (e, w);
    }

    std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;

    /** Sets every note (or selected note) whose bar is between the last mouse position and this one. */
    void drawAt (const juce::MouseEvent& e)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const bool reset = e.mods.isPopupMenu();
        const int value = reset ? getNotePropertyDefault (state.laneProperty) : valueAtY (e.position.y);
        const float from = juce::jmin (lastX, e.position.x) - 3.0f, to = juce::jmax (lastX, e.position.x) + 3.0f;
        const bool onlySelected = ! grid.selected.isEmpty();

        for (auto& n : grid.currentNotes())
        {
            const float x = xForBeat (n[ids::b]) + 1.5f;

            if (x >= from && x <= to && (! onlySelected || grid.selected.contains (n[ids::sid].toString())))
                setNoteProperty (*edit, n, state.laneProperty, value);
        }

        shownValue = nameOf (state.laneProperty) + ": " + valueText (state.laneProperty, value);
        shownAt = e.position;
        repaint();
        grid.repaint();
    }

    void showPropertyMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader (TRANS ("The lane shows each note's"));

        for (auto p : { NoteProperty::velocity, NoteProperty::release, NoteProperty::pan, NoteProperty::fine })
            m.addItem ((int) p + 1, nameOf (p), true, state.laneProperty == p);

        m.addSeparator();
        m.addItem (-1, TRANS ("Pan and fine pitch: built-in Sampler and drum sounds only"), false);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (180),
                         [safe = juce::Component::SafePointer<PropertyLane> (this)] (int r)
        {
            if (safe == nullptr || r < 1 || r > 4)
                return;

            safe->state.laneProperty = (NoteProperty) (r - 1);
            safe->repaint();

            if (safe->onPropertyChanged)
                safe->onPropertyChanged();
        });
    }

    /** A cheap fingerprint of what the lane shows (it repaints when it changes). */
    juce::int64 signature() const
    {
        juce::int64 h = (juce::int64) state.laneProperty * 1000003 + (juce::int64) juce::roundToInt (viewX()) * 31
                        + (juce::int64) juce::roundToInt (grid.pixelsPerBeat * 100.0) + grid.selected.size() * 7919;

        for (auto& n : grid.currentNotes())
            h = h * 131 + juce::roundToInt ((double) n[ids::b] * 1000.0) + getNoteProperty (n, state.laneProperty) * 17;

        return h;
    }

private:
    EditorState& state;
    Grid& grid;
    float lastX = 0.0f;
    juce::String shownValue;
    juce::Point<float> shownAt;
};

//==============================================================================
/** The instrument whose notes you edit: its colour and name; click for the list of
    instruments, the mouse wheel steps through them. */
class PianoRoll::ChannelPicker  : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    explicit ChannelPicker (PianoRoll& o) : owner (o)
    {
        setTooltip (TRANS ("The instrument whose notes you edit here: click for the list (a dot = it has notes in this pattern). "
                           "< > or Alt+Up / Alt+Down go to the previous / next one"));
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    juce::String name;
    juce::Colour colour { theme::colours::muted };

    int idealWidth() const
    {
        return juce::jmin (230, 10 + 10 + 8 + (int) std::ceil (juce::GlyphArrangement::getStringWidth (theme::ui (12.5f, theme::Weight::bold), name)) + 6 + 12 + 10 + 6);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.0f, 3.0f);
        g.setColour (isMouseOver() ? theme::colours::raised : theme::colours::well);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (theme::colours::line);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        auto inner = r.reduced (10.0f, 0.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (inner.removeFromLeft (10.0f).withSizeKeepingCentre (10.0f, 10.0f), 3.0f);
        inner.removeFromLeft (8.0f);
        theme::drawIcon (g, theme::Icon::chevronDown, inner.removeFromRight (12.0f).withSizeKeepingCentre (12.0f, 12.0f), theme::colours::muted);
        inner.removeFromRight (6.0f);
        g.setColour (theme::colours::text);
        g.setFont (theme::ui (12.5f, theme::Weight::bold));
        g.drawText (name, inner, juce::Justification::centredLeft, true);
    }

    void mouseEnter (const juce::MouseEvent&) override   { repaint(); }
    void mouseExit (const juce::MouseEvent&) override    { repaint(); }
    void mouseDown (const juce::MouseEvent&) override    { owner.showChannelMenu(); }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (w.deltaY != 0.0f)
            owner.stepChannel (w.deltaY > 0.0f ? -1 : 1);
    }

private:
    PianoRoll& owner;
};

//==============================================================================
PianoRoll::PianoRoll (EditorState& s) : state (s)
{
    grid = std::make_unique<Grid> (state);
    ruler = std::make_unique<Ruler> (state, *grid);
    keys = std::make_unique<Keys> (state, *grid);
    addAndMakeVisible (*ruler);
    viewport.setViewedComponent (grid.get(), false);
    viewport.setScrollBarsShown (true, true);
    viewport.onMoved = [this] { viewMoved(); };
    addAndMakeVisible (viewport);
    keysHolder.addAndMakeVisible (*keys);
    keysHolder.setInterceptsMouseClicks (false, true);
    addAndMakeVisible (keysHolder);

    grid->onZoom = [this] (float factor, float x) { zoomAround (factor, x); };
    ruler->onZoom = [this] (float factor, float x) { zoomAround (factor, x); };
    ruler->getViewX = [this] { return viewport.getViewPositionX(); };
    grid->onToolKey = [this] { updateTools(); };
    grid->onStepChannel = [this] (int d) { stepChannel (d); };
    grid->onGhostChosen = [this] (const juce::String& channelID)
    {
        state.selectedChannel = channelID;
        state.placeAudioChannel = {};

        if (state.onSelectionChanged)
            state.onSelectionChanged();

        focusNotes();
    };

    // Each note's velocity / release / pan / fine pitch, under the notes.
    lane = std::make_unique<PropertyLane> (state, *grid);
    lane->getViewX = [this] { return viewport.getViewPositionX(); };
    lane->onPropertyChanged = [this] { if (onScaleChanged) onScaleChanged(); };
    addAndMakeVisible (*lane);

    // Ghost notes: the other instruments' notes, faded behind yours.
    ghostButton.setButtonText (TRANS ("Ghosts"));
    ghostButton.setTooltip (TRANS ("Ghost notes: show the other instruments' notes in this pattern, faded behind yours "
                                   "(double-click one to edit that instrument)"));
    theme::setStyle (ghostButton, "toggle");
    ghostButton.setClickingTogglesState (false);
    ghostButton.setWantsKeyboardFocus (false);
    ghostButton.setMouseClickGrabsKeyboardFocus (false);
    ghostButton.onClick = [this]
    {
        state.ghostNotes = ! state.ghostNotes;
        refreshTools();

        if (onScaleChanged)
            onScaleChanged();
    };
    addAndMakeVisible (ghostButton);

    // Which instrument: its name (a menu of all of them) between < and >.
    picker = std::make_unique<ChannelPicker> (*this);
    addAndMakeVisible (*picker);
    theme::setIcon (prevChannelButton, theme::Icon::chevronLeft);
    theme::setIcon (nextChannelButton, theme::Icon::chevronRight);
    prevChannelButton.setTooltip (TRANS ("Previous instrument (Alt+Up)"));
    nextChannelButton.setTooltip (TRANS ("Next instrument (Alt+Down)"));

    for (auto* b : { &prevChannelButton, &nextChannelButton })
    {
        b->getProperties().set ("iconOnly", true);
        theme::setStyle (*b, "flat");
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    prevChannelButton.onClick = [this] { stepChannel (-1); };
    nextChannelButton.onClick = [this] { stepChannel (1); };
    grid->onPreviewChanged = [this] { keys->repaint(); };

    // The wheel over the keyboard or the ruler scrolls the notes.
    auto wheelToView = [this] (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
    {
        viewport.mouseWheelMove (e.getEventRelativeTo (&viewport), w);
    };
    keys->onWheel = wheelToView;
    ruler->onWheel = wheelToView;
    lane->onWheel = wheelToView;

    title.setColour (juce::Label::textColourId, theme::colours::muted);
    title.setFont (theme::ui (11.0f));
    title.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (title);

    // Tools: Pencil, Select, Slice (one of them on, like FL).
    const auto keysHelp = TRANS ("Keys: Ctrl+A all, Ctrl+C / X / V copy / cut / paste, Ctrl+B duplicate, Ctrl+L legato, "
                                 "Ctrl+Q quantize, Delete, arrows move (Ctrl+Up/Down: an octave), Ctrl+wheel zoom");
    pencilButton.setTooltip (TRANS ("Pencil (P): click to add a note, drag it to move it, drag its right edge to change "
                                    "its length; right-click or right-drag deletes. Shift+drag copies, Ctrl+drag selects") + "\n" + keysHelp);
    selectButton.setTooltip (TRANS ("Select (E): drag a box around notes to select them, then drag one of them to move "
                                    "them all. Ctrl+click adds or removes a note, Shift+drag copies") + "\n" + keysHelp);
    sliceButton.setTooltip (TRANS ("Slice (C): click a note, or drag down across several, to cut them in two there") + "\n" + keysHelp);
    theme::setIcon (pencilButton, theme::Icon::pencil);
    theme::setIcon (selectButton, theme::Icon::select);
    theme::setIcon (sliceButton, theme::Icon::slice);

    for (auto* b : { &pencilButton, &selectButton, &sliceButton })
    {
        b->getProperties().set ("iconOnly", true);
        theme::setStyle (*b, "toggle");
        b->setClickingTogglesState (false);
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    pencilButton.onClick = [this] { state.pianoTool = EditorState::PianoTool::pencil; updateTools(); focusNotes(); };
    selectButton.onClick = [this] { state.pianoTool = EditorState::PianoTool::select; updateTools(); focusNotes(); };
    sliceButton.onClick  = [this] { state.pianoTool = EditorState::PianoTool::slice;  updateTools(); focusNotes(); };

    // Snap (the grid notes land on).
    theme::setIcon (snapButton, theme::Icon::magnet);
    theme::setStyle (snapButton, "toggle");
    snapButton.setClickingTogglesState (false);
    snapButton.setWantsKeyboardFocus (false);
    snapButton.setMouseClickGrabsKeyboardFocus (false);
    snapButton.onClick = [this] { showSnapMenu(); };
    addAndMakeVisible (snapButton);

    // Scale helper (off by default: notes go anywhere).
    theme::setIcon (scaleButton, theme::Icon::note);
    theme::setStyle (scaleButton, "toggle");
    scaleButton.setClickingTogglesState (false);
    scaleButton.onClick = [this] { showScaleMenu(); };
    addAndMakeVisible (scaleButton);

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
    updateTools();
    updateSnapButton();
    updateScaleButton();
    startTimerHz (30);
}

void PianoRoll::refreshTools()
{
    ghostButton.setToggleState (state.ghostNotes, juce::dontSendNotification);
    grid->repaint();
    lane->repaint();
}

void PianoRoll::focusNotes()
{
    if (grid->isShowing())
        grid->grabKeyboardFocus();
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

void PianoRoll::updateTools()
{
    pencilButton.setToggleState (state.pianoTool == EditorState::PianoTool::pencil, juce::dontSendNotification);
    selectButton.setToggleState (state.pianoTool == EditorState::PianoTool::select, juce::dontSendNotification);
    sliceButton.setToggleState (state.pianoTool == EditorState::PianoTool::slice, juce::dontSendNotification);
    grid->action = Grid::Action::none;
    grid->setMouseCursor (state.pianoTool == EditorState::PianoTool::slice ? grid->sliceCursor
                            : state.pianoTool == EditorState::PianoTool::pencil ? grid->pencilCursor
                                                                                : juce::MouseCursor (juce::MouseCursor::NormalCursor));
    updateHint();
}

void PianoRoll::updateHint()
{
    auto* channel = state.getSelectedChannel();

    if (channel != nullptr && song::isAudioChannel (*channel))
    {
        title.setText (TRANS ("is an audio channel: move and trim its clips in the playlist"), juce::dontSendNotification);
        return;
    }

    const auto hint = state.pianoTool == EditorState::PianoTool::select ? TRANS ("drag: select, drag a note: move them, Del: delete")
                    : state.pianoTool == EditorState::PianoTool::slice  ? TRANS ("click or drag down across notes: cut them there")
                                                                        : TRANS ("click: add note, drag: move, edge: length, right-click: delete");
    title.setText (TRANS ("in") + " " + state.getPattern()[ids::name].toString() + juce::String::fromUTF8 ("  \xc2\xb7  ") + hint,
                   juce::dontSendNotification);
}

void PianoRoll::updateSnapButton()
{
    snapButton.setButtonText (snapName (state.pianoSnapBeats));
    snapButton.setToggleState (state.pianoSnapBeats != 0.0, juce::dontSendNotification);
    snapButton.setTooltip (TRANS ("Snap: the grid notes land on (hold Alt to place freely)"));
    resized();
}

void PianoRoll::showSnapMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader (TRANS ("Snap notes to (Alt held: free)"));

    for (int i = 0; i < (int) std::size (snapChoices); ++i)
    {
        if (snapChoices[i].beats == 0.0)
            m.addSeparator();

        m.addItem (1 + i, TRANS (snapChoices[i].name), true, std::abs (snapChoices[i].beats - state.pianoSnapBeats) < 1.0e-9);
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (snapButton),
                     [safe = juce::Component::SafePointer<PianoRoll> (this)] (int r)
    {
        if (safe == nullptr || r <= 0)
            return;

        safe->state.pianoSnapBeats = snapChoices[r - 1].beats;
        safe->updateSnapButton();
        safe->grid->repaint();
        safe->focusNotes();

        if (safe->onScaleChanged)
            safe->onScaleChanged();
    });
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
    juce::PopupMenu m, keyMenu, types;

    for (int k = 0; k < 12; ++k)
        keyMenu.addItem (100 + k, scales::keyName (k), true, k == state.scaleKey);

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
    m.addSubMenu (TRANS ("Key") + ": " + scales::keyName (state.scaleKey), keyMenu);
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
        safe->keys->repaint();

        if (safe->onScaleChanged)
            safe->onScaleChanged();
    });
}

/** The instruments (not audio channels), in rack order. */
static juce::Array<te::AudioTrack*> instrumentChannels (te::Edit& edit)
{
    juce::Array<te::AudioTrack*> result;

    for (auto* t : song::getChannels (edit))
        if (! song::isAudioChannel (*t))
            result.add (t);

    return result;
}

void PianoRoll::stepChannel (int direction)
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    auto list = instrumentChannels (*edit);

    if (list.isEmpty())
        return;

    const int now = list.indexOf (state.getSelectedChannel());
    const int next = now < 0 ? 0 : juce::jlimit (0, list.size() - 1, now + direction);

    if (next == now)
        return;

    state.selectedChannel = song::getChannelID (*list[next]);
    state.placeAudioChannel = {};

    if (state.onSelectionChanged)
        state.onSelectionChanged();

    focusNotes();
}

void PianoRoll::showChannelMenu()
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    auto list = instrumentChannels (*edit);
    auto all = song::getChannels (*edit);
    auto pattern = state.getPattern();
    juce::PopupMenu m;
    m.addSectionHeader (TRANS ("Edit the notes of") + " (" + pattern[ids::name].toString() + ")");

    for (int i = 0; i < list.size(); ++i)
    {
        auto* t = list[i];
        const bool hasNotes = ! getNotes (pattern, getChannelID (*t)).isEmpty();
        m.addColouredItem (1 + i, juce::String::fromUTF8 ("\xe2\x96\xa0  ") + t->getName()
                                    + (hasNotes ? juce::String::fromUTF8 ("  \xe2\x80\xa2") : juce::String()),
                           EditorState::channelColour (all.indexOf (t)), true, t == state.getSelectedChannel());
    }

    if (list.isEmpty())
        m.addItem (-1, TRANS ("(no instruments in the rack)"), false);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (picker.get()),
                     [safe = juce::Component::SafePointer<PianoRoll> (this), list] (int r)
    {
        if (safe == nullptr || r <= 0 || r > list.size())
            return;

        auto* e = safe->state.getEdit();

        // (Still there? It may have been deleted while the menu was open.)
        if (e == nullptr || ! song::getChannels (*e).contains (list[r - 1]))
            return;

        safe->state.selectedChannel = song::getChannelID (*list[r - 1]);
        safe->state.placeAudioChannel = {};

        if (safe->state.onSelectionChanged)
            safe->state.onSelectionChanged();

        safe->focusNotes();
    });
}

PianoRoll::~PianoRoll()
{
    stopTimer();
}

void PianoRoll::refresh()
{
    auto* channel = state.getSelectedChannel();

    heading = channel != nullptr ? channel->getName() : TRANS ("no channel");
    headingColour = channel != nullptr && state.getEdit() != nullptr
                      ? EditorState::channelColour (song::getChannels (*state.getEdit()).indexOf (channel))
                      : theme::colours::muted;
    picker->name = heading;
    picker->colour = headingColour;
    picker->repaint();

    grid->checkContext();
    updateHint();
    updateScaleButton();   // (also lays out: the scale may have been set from your settings)
    updateSnapButton();
    repaint (getLocalBounds().removeFromTop (titleHeight));
    grid->repaint();
    ruler->repaint();
    keys->repaint();
    lane->repaint();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (theme::colours::wellAlt);

    // (The instrument's name and colour are on the picker in the title row.)
}

void PianoRoll::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (titleHeight).reduced (12, 0);

    // Right side of the title row: tools, snap, scale, own window, full screen.
    auto buttons = top.reduced (0, 3);
    fullScreenButton.setBounds (buttons.removeFromRight (30));
    buttons.removeFromRight (4);
    detachButton.setBounds (buttons.removeFromRight (30));
    buttons.removeFromRight (8);
    const int scaleWidth = (int) juce::GlyphArrangement::getStringWidth (theme::ui (13.0f, theme::Weight::semibold), scaleButton.getButtonText()) + 15 + 6 + 26;
    scaleButton.setBounds (buttons.removeFromRight (juce::jmin (220, scaleWidth)));
    buttons.removeFromRight (6);
    const int snapWidth = (int) juce::GlyphArrangement::getStringWidth (theme::ui (13.0f, theme::Weight::semibold), snapButton.getButtonText()) + 15 + 6 + 26;
    snapButton.setBounds (buttons.removeFromRight (juce::jmin (140, snapWidth)));
    buttons.removeFromRight (6);
    ghostButton.setBounds (buttons.removeFromRight (juce::jmax (58, (int) juce::GlyphArrangement::getStringWidth (theme::ui (13.0f, theme::Weight::semibold), ghostButton.getButtonText()) + 20)));
    buttons.removeFromRight (10);
    sliceButton.setBounds (buttons.removeFromRight (30));
    buttons.removeFromRight (2);
    selectButton.setBounds (buttons.removeFromRight (30));
    buttons.removeFromRight (2);
    pencilButton.setBounds (buttons.removeFromRight (30));
    top.setRight (pencilButton.getX() - 10);

    // Left: < [instrument] > then the hint.
    auto left = top.reduced (0, 2);
    prevChannelButton.setBounds (left.removeFromLeft (24));
    left.removeFromLeft (2);
    picker->setBounds (left.removeFromLeft (picker->idealWidth()));
    left.removeFromLeft (2);
    nextChannelButton.setBounds (left.removeFromLeft (24));
    title.setBounds (top.withLeft (nextChannelButton.getRight() + 10));
    ruler->setBounds (r.removeFromTop (20).withTrimmedRight (viewport.getScrollBarThickness()));

    // The note-property lane, when there's room for it.
    lane->setVisible (r.getHeight() > 200);

    if (lane->isVisible())
        lane->setBounds (r.removeFromBottom (laneHeight).withTrimmedRight (viewport.getScrollBarThickness()));

    keysHolder.setBounds (r.removeFromLeft (keyboardWidth));
    viewport.setBounds (r);
    updateGridSize();

    if (! scrolledOnce && viewport.getHeight() > 0)
    {
        // Centre on C5 (MIDI 60, FL naming), where steps put their notes.
        scrolledOnce = true;
        viewport.setViewPosition (0, juce::jmax (0, (int) grid->yForPitch (60) - viewport.getHeight() / 2));
    }

    viewMoved();
}

void PianoRoll::updateGridSize()
{
    const int visibleWidth = juce::jmax (50, viewport.getWidth() - viewport.getScrollBarThickness());
    const double length = juce::jmax (1.0, song::getLengthBeats (state.getPattern()));
    const double fit = visibleWidth / length;

    // 1 = the pattern fits; zoomed in up to 400 pixels a beat (a step = 100).
    zoom = juce::jlimit (1.0, juce::jmax (1.0, 400.0 / fit), zoom);
    grid->pixelsPerBeat = fit * zoom;
    sizedForLength = length;
    grid->setSize (juce::jmax (visibleWidth, (int) std::ceil (length * grid->pixelsPerBeat)), grid->getHeight());
    grid->repaint();
    ruler->repaint();
}

void PianoRoll::zoomAround (float factor, float gridX)
{
    // Keep the beat under the mouse where it is.
    const double beat = gridX / grid->pixelsPerBeat;
    const int inView = juce::roundToInt (gridX) - viewport.getViewPositionX();
    zoom *= factor;
    updateGridSize();
    viewport.setViewPosition (juce::jmax (0, juce::roundToInt (beat * grid->pixelsPerBeat) - inView), viewport.getViewPositionY());
    viewMoved();
}

void PianoRoll::viewMoved()
{
    // The keyboard follows the notes up and down; the ruler follows them sideways.
    keys->setTopLeftPosition (0, -viewport.getViewPositionY());
    keysHolder.setSize (keyboardWidth, juce::jmin (viewport.getHeight(), viewport.getViewHeight()));
    ruler->repaint();
    lane->repaint();
}

void PianoRoll::timerCallback()
{
    // The lane follows the notes (edited here or by someone else).
    if (lane->isVisible())
    {
        const auto sig = lane->signature();

        if (sig != laneSignature)
        {
            laneSignature = sig;
            lane->repaint();
        }
    }

    // The pattern got longer or shorter (here or from someone else): re-size the grid.
    if (std::abs (song::getLengthBeats (state.getPattern()) - sizedForLength) > 1.0e-9)
        updateGridSize();

    // Smooth playhead while playing; one last repaint when it stops.
    const int playhead = state.getPlayheadStep();
    const double playBeat = state.getPlayheadBeat();

    if (playhead >= 0 || playhead != lastPlayhead)
    {
        lastPlayhead = playhead;

        // Zoomed in: the view turns the page when the playhead leaves it (not while you drag).
        if (playBeat >= 0.0 && grid->getWidth() > viewport.getViewWidth()
             && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        {
            const int x = juce::roundToInt (grid->xForBeat (playBeat));

            if (x < viewport.getViewPositionX() || x > viewport.getViewPositionX() + viewport.getViewWidth() - 8)
                viewport.setViewPosition (juce::jmax (0, x - 16), viewport.getViewPositionY());
        }

        grid->repaint (viewport.getViewArea());
        ruler->repaint();
    }
}

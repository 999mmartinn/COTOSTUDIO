#include "Playlist.h"
#include "FileImport.h"

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
class Playlist::Lanes  : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        public juce::DragAndDropTarget,
                        public juce::ChangeListener
{
public:
    Lanes (Playlist& p) : owner (p), state (p.state) {}

    // Things dragged in from inside the app: an audio channel (channel rack),
    // a pattern or an audio file (browser). Each is "kind:id".
    struct Dragged { juce::String kind, id; };

    static Dragged dragged (const SourceDetails& d)
    {
        const auto text = d.description.toString();

        for (auto* kind : { "audioChannel", "pattern", "asset", "file" })
            if (text.startsWith (juce::String (kind) + ":"))
                return { kind, text.fromFirstOccurrenceOf (":", false, false) };

        return {};
    }

    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        const auto what = dragged (d);

        // From the sound browser: sound files become audio clips (MIDI goes on the channel rack).
        if (what.kind == "file")
            return fileimport::isAudioFile (juce::File (what.id));

        return what.kind.isNotEmpty();
    }

    /** Length of a sound file not in the song yet (read once per file while dragging). */
    double secondsOfFile (const juce::String& path)
    {
        if (path != lengthCachePath)
        {
            lengthCachePath = path;
            lengthCacheSeconds = 0.0;

            if (std::unique_ptr<juce::AudioFormatReader> r { owner.formats.createReaderFor (juce::File (path)) })
                if (r->sampleRate > 0.0)
                    lengthCacheSeconds = (double) r->lengthInSamples / r->sampleRate;
        }

        return lengthCacheSeconds;
    }

    juce::String lengthCachePath;
    double lengthCacheSeconds = 0.0;

    void itemDragMove (const SourceDetails& d) override
    {
        const auto what = dragged (d);
        dropPreview = { laneAtY (d.localPosition.y), snap (beatAtX (d.localPosition.x)), what.kind, what.id };
        repaint();
    }

    void itemDragExit (const SourceDetails&) override   { dropPreview = {}; repaint(); }

    /** How long (in beats) the dragged thing would be at this spot, or 0. */
    double previewLength (te::Edit& edit)
    {
        const double beat = dropPreview.beat;

        auto lengthOfFile = [&] (const juce::String& hash)
        {
            const double seconds = juce::jmax (0.05, getAssetSeconds (edit, hash));
            return secondsToBeats (edit, beatsToSeconds (edit, beat) + seconds) - beat;
        };

        if (dropPreview.kind == "audioChannel")
            if (auto* t = findChannel (edit, dropPreview.id))
                return lengthOfFile (getChannelAsset (edit, *t));

        if (dropPreview.kind == "asset")
            return lengthOfFile (dropPreview.id);

        if (dropPreview.kind == "file")
        {
            const double seconds = juce::jmax (0.05, secondsOfFile (dropPreview.id));
            return secondsToBeats (edit, beatsToSeconds (edit, beat) + seconds) - beat;
        }

        if (dropPreview.kind == "pattern")
        {
            auto pattern = findPattern (edit, dropPreview.id);
            return pattern.isValid() ? getLengthBeats (pattern) : 0.0;
        }

        return 0.0;
    }

    void itemDropped (const SourceDetails& d) override
    {
        dropPreview = {};
        auto* edit = state.getEdit();
        const auto what = dragged (d);
        const int lane = laneAtY (d.localPosition.y);
        const double beat = snap (beatAtX (d.localPosition.x));

        if (edit == nullptr)
            return;

        if (what.kind == "file")
        {
            // A sound from your folders: it joins the song (and is sent to the others) now.
            owner.importFiles ({ juce::File (what.id) }, lane, beat);
            repaint();
            return;
        }

        if (what.kind == "pattern")
        {
            if (findPattern (*edit, what.id).isValid())
            {
                state.beginAction();
                addClip (*edit, what.id, lane, beat);
                state.viewedPattern = what.id;
                state.placeAudioChannel = {};
            }
        }
        else
        {
            state.beginAction();
            te::AudioTrack* channel = what.kind == "asset" ? findOrCreateChannelForAsset (*edit, what.id)
                                                           : findChannel (*edit, what.id);

            if (channel != nullptr)
            {
                placeAudioChannel (*edit, *channel, lane, beat);
                state.selectedChannel = state.placeAudioChannel = getChannelID (*channel);
            }
        }

        if (state.onSelectionChanged)
            state.onSelectionChanged();

        repaint();
    }

    // Audio files dragged in from Explorer
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
                return true;

        return false;
    }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        juce::Array<juce::File> list;

        for (auto& f : files)
            if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
                list.add (juce::File (f));

        owner.importFiles (list, laneAtY ((float) y), snap (beatAtX ((float) x)));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override   { repaint(); }   // a waveform finished loading

    float xForBeat (double beat) const   { return (float) (beat * owner.pixelsPerBeat); }
    double beatAtX (float x) const       { return juce::jmax (0.0, (double) x / owner.pixelsPerBeat); }
    int laneAtY (float y) const          { return juce::jlimit (0, numLanes - 1, (int) (y / laneHeight)); }
    double snap (double beat) const        { return state.snapPlaylistBeat (beat); }
    double snapNearest (double beat) const { return state.snapPlaylistBeatNearest (beat); }

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
            auto r = clipBounds (clip).reduced (0.5f, 1.5f);

            if (! r.intersects (visible.toFloat()))
                continue;

            if (isAudioClip (clip))
            {
                paintAudioClip (g, *edit, clip, r);
                continue;
            }

            if (isAutomationClip (clip))
            {
                paintAutomationClip (g, *edit, clip, r);
                continue;
            }

            auto pattern = findPattern (*edit, clip[ids::pattern].toString());

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

        // Where something dragged in (rack or browser) would land
        if (dropPreview.lane >= 0)
        {
            const double length = previewLength (*edit);

            if (length > 0.0)
            {
                const juce::Rectangle<float> r (xForBeat (dropPreview.beat), (float) (dropPreview.lane * laneHeight) + 1.5f,
                                                xForBeat (dropPreview.beat + length) - xForBeat (dropPreview.beat),
                                                (float) laneHeight - 3.0f);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (juce::Colours::white.withAlpha (0.8f));
                g.drawRoundedRectangle (r, 3.0f, 1.5f);
            }
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

    void paintAutomationClip (juce::Graphics& g, te::Edit& edit, const juce::ValueTree& clip, juce::Rectangle<float> r)
    {
        const juce::Colour colour (0xff45c4b0);
        const bool selected = state.selectedAutomation == clip[ids::sid].toString();
        g.setColour (colour.withAlpha (0.18f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour);
        g.drawRoundedRectangle (r, 3.0f, selected ? 2.0f : 1.0f);

        // The curve across the clip.
        const double len = getClipLength (edit, clip);
        auto body = r.reduced (2.0f, 3.0f).withTrimmedTop (9.0f);
        juce::Path path;
        const int steps = juce::jlimit (2, 400, (int) r.getWidth() / 3);

        for (int i = 0; i <= steps; ++i)
        {
            const double b = len * i / steps;
            const float x = body.getX() + body.getWidth() * (float) i / (float) steps;
            const float y = body.getBottom() - getAutomationValueAt (clip, b) * body.getHeight();

            if (i == 0) path.startNewSubPath (x, y);
            else        path.lineTo (x, y);
        }

        g.setColour (colour.brighter (0.3f));
        g.strokePath (path, juce::PathStrokeType (1.5f));

        for (auto& p : getAutomationPoints (clip))
            if ((double) p[ids::b] <= len)
                g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f)
                                 .withCentre ({ body.getX() + body.getWidth() * (float) ((double) p[ids::b] / len),
                                                body.getBottom() - (float) p[ids::v] * body.getHeight() }));

        g.setColour (juce::Colours::white);
        g.setFont (10.0f);
        g.drawText (describeAutomationTarget (edit, clip), r.reduced (4.0f, 0.0f).withHeight (11.0f).toNearestInt(),
                    juce::Justification::centredLeft, true);
    }

    void paintAudioClip (juce::Graphics& g, te::Edit& edit, const juce::ValueTree& clip, juce::Rectangle<float> r)
    {
        const auto hash = clip[ids::asset].toString();
        const auto channelID = clip[ids::ch].toString();
        auto channels = getChannels (edit);
        int index = 0;

        for (int i = 0; i < channels.size(); ++i)
            if (getChannelID (*channels[i]) == channelID)
                index = i;

        const auto colour = EditorState::channelColour (index);
        const bool selected = state.selectedChannel == channelID;

        g.setColour (colour.withAlpha (0.25f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour);
        g.drawRoundedRectangle (r, 3.0f, selected ? 2.0f : 1.0f);

        auto& store = state.session.getAssetStore();
        auto body = r.reduced (2.0f, 2.0f);

        if (store.has (hash))
        {
            if (auto* thumb = owner.getThumbnail (hash))
            {
                // Left channel at full clip height: easier to read than two thin strips.
                const double offset = clip[ids::offset], duration = clip[ids::duration];
                g.setColour (colour.brighter (0.4f).withAlpha (0.9f));
                thumb->drawChannel (g, body.toNearestInt(), offset, offset + duration, 0, 1.0f);
            }
        }
        else
        {
            const double progress = store.getProgress (hash);
            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.setFont (11.0f);
            g.drawText (progress >= 0.0 ? "receiving " + juce::String (juce::roundToInt (progress * 100.0)) + "%"
                                        : juce::String ("waiting for the file..."),
                        body.withTrimmedTop (12.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }

        // Name on a small dark tab so it stays readable over the waveform.
        const auto name = getAssetName (edit, hash);
        juce::Font font (juce::FontOptions (11.0f));
        const float tabWidth = juce::jmin (r.getWidth() - 2.0f, juce::GlyphArrangement::getStringWidth (font, name) + 12.0f);
        auto tab = juce::Rectangle<float> (r.getX() + 1.0f, r.getY() + 1.0f, tabWidth, 12.0f);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (tab, 2.0f);
        g.setColour (juce::Colours::white);
        g.setFont (font);
        g.drawText (name, tab.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto c = clipAt (e.position);
        const bool onEdge = c.isValid() && (e.position.x > clipBounds (c).getRight() - 6.0f
                                             || (isAudioClip (c) && e.position.x < clipBounds (c).getX() + 6.0f));
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
            const auto bounds = clipBounds (hit);
            dragMode = e.position.x > bounds.getRight() - 6.0f ? DragMode::resize
                     : (isAudioClip (hit) && e.position.x < bounds.getX() + 6.0f) ? DragMode::trimStart
                     : DragMode::move;
            grabStart = hit[ids::start];
            grabBeat = beatAtX (e.position.x);
            grabOffset = hit[ids::offset];
            grabDuration = hit[ids::duration];
            clickedExisting = true;
        }
        else
        {
            // Place what is selected: an audio channel's file, or else the viewed pattern.
            if (auto* audio = state.getPlaceAudioChannel())
            {
                dragClip = placeAudioChannel (*edit, *audio, laneAtY (e.position.y), snap (beatAtX (e.position.x)));
            }
            else
            {
                auto pattern = state.getPattern();

                if (! pattern.isValid())
                    return;

                dragClip = addClip (*edit, pattern[ids::sid].toString(), laneAtY (e.position.y), snap (beatAtX (e.position.x)));
            }

            if (! dragClip.isValid())
                return;

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
            const double newStart = juce::jmax (0.0, snapNearest (grabStart + beatAtX (e.position.x) - grabBeat));
            const int newLane = laneAtY (e.position.y);

            if (std::abs (newStart - (double) dragClip[ids::start]) > 1.0e-6)
                dragClip.setProperty (ids::start, newStart, um);

            if (newLane != (int) dragClip[ids::lane])
                dragClip.setProperty (ids::lane, newLane, um);
        }
        else if (isAudioClip (dragClip))
        {
            // Audio clips are trimmed in seconds, so tempo changes never stretch them.
            const double fileSeconds = juce::jmax (0.05, getAssetSeconds (*edit, dragClip[ids::asset].toString()));
            const double startSeconds = beatsToSeconds (*edit, grabStart);

            if (dragMode == DragMode::resize)
            {
                const double endBeat = snapNearest (beatAtX (e.position.x));
                const double duration = juce::jlimit (0.05, juce::jmax (0.05, fileSeconds - grabOffset), beatsToSeconds (*edit, endBeat) - startSeconds);

                if (std::abs (duration - (double) dragClip[ids::duration]) > 1.0e-6)
                    dragClip.setProperty (ids::duration, duration, um);
            }
            else   // trimStart: move the left edge, keeping the right edge where it is
            {
                const double endSeconds = startSeconds + grabDuration;
                double newStartSeconds = beatsToSeconds (*edit, snapNearest (beatAtX (e.position.x)));
                newStartSeconds = juce::jlimit (startSeconds - grabOffset, endSeconds - 0.05, newStartSeconds);
                newStartSeconds = juce::jmax (0.0, newStartSeconds);

                const double newOffset = grabOffset + (newStartSeconds - startSeconds);
                const double newStart = secondsToBeats (*edit, newStartSeconds);

                if (std::abs (newStart - (double) dragClip[ids::start]) > 1.0e-6)
                {
                    dragClip.setProperty (ids::start, newStart, um);
                    dragClip.setProperty (ids::offset, juce::jmax (0.0, newOffset), um);
                    dragClip.setProperty (ids::duration, endSeconds - newStartSeconds, um);
                }
            }
        }
        else
        {
            const double start = dragClip[ids::start];
            const double end = juce::jmax (start + stepBeats, snapNearest (beatAtX (e.position.x)));

            if (std::abs (end - start - getClipLength (*edit, dragClip)) > 1.0e-6)
                dragClip.setProperty (ids::length, end - start, um);
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // A click (no drag) on a clip opens its pattern for editing, or selects
        // an audio clip's channel.
        if (dragClip.isValid() && clickedExisting && ! moved)
        {
            if (isAutomationClip (dragClip))
            {
                state.selectedAutomation = dragClip[ids::sid].toString();
                dragClip = {};

                if (state.onAutomationSelected)
                    state.onAutomationSelected();

                return;
            }

            if (isAudioClip (dragClip))
            {
                state.selectedChannel = dragClip[ids::ch].toString();
                state.placeAudioChannel = state.selectedChannel;
            }
            else
            {
                state.viewedPattern = dragClip[ids::pattern].toString();
                state.placeAudioChannel = {};
            }

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
    struct DropPreview { int lane = -1; double beat = 0.0; juce::String kind, id; } dropPreview;
    enum class DragMode { move, resize, trimStart } dragMode = DragMode::move;
    double grabStart = 0.0, grabBeat = 0.0, grabOffset = 0.0, grabDuration = 0.0;
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
        state.songStartBeat = state.snapPlaylistBeat (beat);

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

    formats.registerBasicFormats();

    importButton.setTooltip ("Add audio files (WAV, MP3, FLAC...) to the song. You can also drag them onto a lane");
    importButton.onClick = [this] { showImportDialog(); };
    addAndMakeVisible (importButton);

    automateButton.setTooltip ("Make an automation clip for any knob: the one you moved last, or pick one "
                               "(channels, mixer, plugins). Automation plays in SONG mode");
    automateButton.onClick = [this] { showAutomateMenu(); };
    addAndMakeVisible (automateButton);

    snapBox.addItem ("Snap: bar", 1);
    snapBox.addItem ("Snap: beat", 2);
    snapBox.addItem ("Snap: step", 3);
    snapBox.addItem ("Snap: none", 4);
    snapBox.setSelectedId (1, juce::dontSendNotification);
    snapBox.setTooltip ("Grid that clips snap to. Hold Alt while dragging to move freely");
    snapBox.onChange = [this]
    {
        const int id = snapBox.getSelectedId();
        state.playlistSnapBeats = id == 1 ? 4.0 : id == 2 ? 1.0 : id == 3 ? stepBeats : 0.0;
    };
    addAndMakeVisible (snapBox);

    startTimerHz (30);
}

Playlist::~Playlist()
{
    stopTimer();
    importPool.removeAllJobs (true, 30000);

    for (auto& [hash, thumb] : thumbnails)
        thumb->removeAllChangeListeners();
}

void Playlist::showAutomateMenu()
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    // Every knob that can be automated, grouped like the app: channels, then the mixer.
    std::vector<std::pair<juce::String, juce::String>> targets;   // pluginID, paramID
    juce::PopupMenu menu, channels, mixer;

    auto addTarget = [&targets] (juce::PopupMenu& m, const juce::String& text, const juce::String& pluginID, const juce::String& paramID)
    {
        targets.push_back ({ pluginID, paramID });
        m.addItem ((int) targets.size(), text);
    };

    auto addPluginKnobs = [&] (juce::PopupMenu& m, te::Plugin& plugin)
    {
        if (isGeneratedPlugin (&plugin) || dynamic_cast<te::LevelMeterPlugin*> (&plugin) != nullptr)
            return;

        if (auto* vp = dynamic_cast<te::VolumeAndPanPlugin*> (&plugin))
        {
            addTarget (m, "Volume", vp->itemID.toString(), "volume");
            addTarget (m, "Pan", vp->itemID.toString(), "pan");
            return;
        }

        juce::PopupMenu knobs;

        for (auto* param : plugin.getAutomatableParameters())
            addTarget (knobs, param->getParameterName(), plugin.itemID.toString(), param->paramID);

        if (knobs.getNumItems() > 0)
            m.addSubMenu (plugin.getName(), knobs);
    };

    for (auto* t : getChannels (*edit))
    {
        juce::PopupMenu sub;

        for (auto* p : t->pluginList.getPlugins())
            addPluginKnobs (sub, *p);

        channels.addSubMenu (t->getName(), sub);
    }

    {
        juce::PopupMenu master;

        if (auto vp = edit->getMasterVolumePlugin())
            addPluginKnobs (master, *vp);

        for (auto* p : getMasterEffects (*edit))
            addPluginKnobs (master, *p);

        mixer.addSubMenu ("Master", master);
    }

    for (auto* insert : getInserts (*edit))
    {
        juce::PopupMenu sub;

        for (auto* p : insert->pluginList.getPlugins())
            addPluginKnobs (sub, *p);

        mixer.addSubMenu (juce::String (getInsertNumber (*edit, *insert)) + " " + insert->getName(), sub);
    }

    // The knob you moved last (ours, or a plugin's).
    for (auto& last : { state.lastTweaked, state.session.getSync() != nullptr ? state.session.getSync()->getLastTweakedParameter()
                                                                              : juce::String() })
    {
        const auto pluginID = last.upToFirstOccurrenceOf ("|", false, false), paramID = last.fromFirstOccurrenceOf ("|", false, false);

        if (last.isNotEmpty() && findParameter (*edit, pluginID, paramID) != nullptr)
            addTarget (menu, "Last moved: " + describeParameter (*edit, pluginID, paramID), pluginID, paramID);
    }

    if (menu.getNumItems() == 0)
        menu.addItem (-1, "(move a knob, then come back here to automate it)", false);

    menu.addSeparator();
    menu.addSubMenu ("Channels", channels);
    menu.addSubMenu ("Mixer", mixer);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (automateButton), [this, targets] (int r)
    {
        if (r >= 1 && r <= (int) targets.size() && state.createAutomation)
            state.createAutomation (targets[(size_t) r - 1].first, targets[(size_t) r - 1].second);
    });
}

void Playlist::showImportDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || fc.getResults().isEmpty())
            return;

        // First lane that is free at the song start marker.
        int lane = 0;

        for (; lane < numLanes - 1; ++lane)
        {
            bool busy = false;

            for (auto& c : getClips (*edit))
                if ((int) c[ids::lane] == lane && (double) c[ids::start] <= state.songStartBeat
                     && (double) c[ids::start] + getClipLength (*edit, c) > state.songStartBeat)
                    busy = true;

            if (! busy)
                break;
        }

        importFiles (fc.getResults(), lane, state.songStartBeat);
    });
}

juce::AudioThumbnail* Playlist::getThumbnail (const juce::String& hash)
{
    auto it = thumbnails.find (hash);

    if (it != thumbnails.end())
        return it->second.get();

    auto& store = state.session.getAssetStore();

    if (! store.has (hash))
        return nullptr;

    auto thumb = std::make_unique<juce::AudioThumbnail> (256, formats, thumbCache);
    thumb->setSource (new juce::FileInputSource (store.getFile (hash)));
    thumb->addChangeListener (lanes.get());
    return thumbnails.emplace (hash, std::move (thumb)).first->second.get();
}

void Playlist::importFiles (const juce::Array<juce::File>& files, int lane, double startBeat)
{
    for (int i = 0; i < files.size(); ++i)
    {
        const auto file = files[i];
        const int targetLane = juce::jmin (numLanes - 1, lane + i);
        auto* store = &state.session.getAssetStore();
        state.session.logMessage ("Importing " + file.getFileName() + "...");

        juce::Component::SafePointer<Playlist> safePlaylist (this);   // made here, on the message thread

        importPool.addJob ([safePlaylist, file, targetLane, startBeat, store]
        {
            auto result = store->importAudioFile (file);   // converts to FLAC (background thread)

            juce::MessageManager::callAsync ([safe = safePlaylist, result, file, targetLane, startBeat]
            {
                if (safe == nullptr)
                    return;

                auto* edit = safe->state.getEdit();

                if (! result.ok() || edit == nullptr)
                {
                    safe->state.session.logMessage (result.error.isNotEmpty() ? result.error : "Import failed");
                    return;
                }

                safe->state.beginAction();
                auto clip = addImportedAudio (*edit, result, file.getFileName(), targetLane, startBeat);

                if (clip.isValid())
                {
                    safe->state.selectedChannel = clip[ids::ch].toString();
                    safe->state.placeAudioChannel = safe->state.selectedChannel;
                    safe->state.session.logMessage ("Added " + file.getFileName() + " ("
                                                     + juce::String (result.lengthSeconds, 1) + " s) - it is being sent to the others");

                    if (safe->state.onSelectionChanged)
                        safe->state.onSelectionChanged();
                }
            });
        });
    }
}

void Playlist::refresh()
{
    auto* audio = state.getPlaceAudioChannel();
    title.setText ("Playlist - click to place \"" + (audio != nullptr ? audio->getName() : state.getPattern()[ids::name].toString())
                     + "\", drag to move, drag edges to resize, right-click to delete, Alt: no snap, drop audio files here",
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
    top.removeFromRight (6);
    importButton.setBounds (top.removeFromRight (120));
    top.removeFromRight (6);
    automateButton.setBounds (top.removeFromRight (100));
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

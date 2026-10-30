#include "Playlist.h"
#include "FileImport.h"

using namespace song;

static juce::Colour patternColour (const juce::ValueTree& pattern)
{
    auto c = pattern[ids::colour].toString();
    return c.isNotEmpty() ? juce::Colour::fromString (c) : theme::colours::mint;
}

/** Colours offered for patterns and playlist tracks (FL-style palette in our look). */
struct NamedColour { juce::Colour colour; const char* name; };

static const NamedColour palette[] =
{
    { theme::colours::red,    NEEDS_TRANS ("Red") },
    { theme::colours::orange, NEEDS_TRANS ("Orange") },
    { theme::colours::amber,  NEEDS_TRANS ("Yellow") },
    { theme::colours::mint,   NEEDS_TRANS ("Green") },
    { theme::colours::teal,   NEEDS_TRANS ("Teal") },
    { theme::colours::sky,    NEEDS_TRANS ("Blue") },
    { theme::colours::violet, NEEDS_TRANS ("Violet") },
    { theme::colours::lilac,  NEEDS_TRANS ("Lilac") },
    { theme::colours::pink,   NEEDS_TRANS ("Pink") },
    { juce::Colour (0xff8a8f98), NEEDS_TRANS ("Grey") },
};

/** A colour submenu: items firstID + index; noneID = "no colour / default". */
static juce::PopupMenu colourMenu (int firstID, int noneID, const juce::String& noneText, juce::Colour current)
{
    juce::PopupMenu m;

    for (int i = 0; i < (int) std::size (palette); ++i)
        m.addColouredItem (firstID + i, juce::String::fromUTF8 ("\xe2\x96\xa0  ") + TRANS (palette[i].name), palette[i].colour,
                           true, current.withAlpha (1.0f) == palette[i].colour.withAlpha (1.0f));

    m.addSeparator();
    m.addItem (noneID, noneText, true, current.isTransparent());
    return m;
}

static void askForText (const juce::String& title, const juce::String& message, const juce::String& initial,
                        std::function<void (juce::String)> onOK)
{
    auto* w = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", initial);
    w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, onOK] (int result)
    {
        if (result == 1)
            onOK (w->getTextEditorContents ("text").trim());
    }), true);
}

/** A take was put on a playlist track that sends takes to an insert: route it there, and say so. */
static void routeTakeAndTell (EditorState& state, const juce::ValueTree& clip)
{
    auto* edit = state.getEdit();

    if (edit == nullptr || ! routeTakeToLane (*edit, clip))
        return;

    if (auto* channel = findChannel (*edit, clip[ids::ch].toString()))
    {
        auto* insert = resolveRoute (*edit, *channel);
        state.session.logMessage (TRANS ("{TAKE} now plays through {INSERT} (the track it is on)")
                                    .replace ("{TAKE}", channel->getName())
                                    .replace ("{INSERT}", insert != nullptr ? insert->getName() : TRANS ("Master")));
    }
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
                routeTakeAndTell (state, placeAudioChannel (*edit, *channel, lane, beat));
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
    int laneAtY (float y) const          { return owner.laneAtRowY ((int) y); }
    /** True over a group's header row (nothing is placed there). */
    bool isGroupRowAt (float y) const    { return ! owner.rows.empty() && owner.rows[(size_t) owner.rowAtY ((int) y)].lane < 0; }
    double snap (double beat) const        { return state.snapPlaylistBeat (beat); }
    double snapNearest (double beat) const { return state.snapPlaylistBeatNearest (beat); }

    juce::Rectangle<float> clipBounds (const juce::ValueTree& clip) const
    {
        auto* edit = state.getEdit();
        const double start = clip[ids::start];
        const double len = edit != nullptr ? getClipLength (*edit, clip) : 4.0;
        const int lane = juce::jlimit (0, numLanes - 1, (int) clip[ids::lane]);
        const float x = xForBeat (start), w = xForBeat (start + len) - xForBeat (start);

        // In a folded group: a thin strip per track inside the group's header row.
        if (owner.isLaneHidden (lane))
        {
            const auto& row = owner.rows[(size_t) owner.rowOfLane[(size_t) lane]];
            const float inner = (float) laneHeight - 8.0f;
            const float h = inner / (float) juce::jmax (1, row.group.count);
            return { x, (float) owner.laneTop (lane) + 6.0f + h * (float) (lane - row.group.first), w, juce::jmax (2.0f, h) };
        }

        return { x, (float) owner.laneTop (lane), w, (float) laneHeight };
    }

    juce::ValueTree clipAt (juce::Point<float> p) const
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return {};

        auto clips = getClips (*edit);

        for (int i = clips.size(); --i >= 0;)
            if (! owner.isLaneHidden (clips[i][ids::lane]) && clipBounds (clips[i]).contains (p))   // (a folded group's clips can't be grabbed)
                return clips[i];

        return {};
    }

    //==============================================================================
    // Audio clip fades: a small square at the top of the clip where each fade ends.
    enum class FadeHandle { none, in, out };

    float fadeInX (te::Edit& edit, const juce::ValueTree& clip) const
    {
        const double startSeconds = beatsToSeconds (edit, clip[ids::start]);
        return xForBeat (secondsToBeats (edit, startSeconds + getClipFadeIn (clip)));
    }

    float fadeOutX (te::Edit& edit, const juce::ValueTree& clip) const
    {
        const double endSeconds = beatsToSeconds (edit, clip[ids::start]) + (double) clip[ids::duration];
        return xForBeat (secondsToBeats (edit, endSeconds - getClipFadeOut (clip)));
    }

    static juce::Rectangle<float> fadeHandleBox (float x, juce::Rectangle<float> clipArea)
    {
        const float size = 7.0f;
        const float left = juce::jlimit (clipArea.getX() + 1.0f, juce::jmax (clipArea.getX() + 1.0f, clipArea.getRight() - size - 1.0f),
                                         x - size * 0.5f);
        return { left, clipArea.getY() + 2.0f, size, size };
    }

    FadeHandle fadeHandleAt (const juce::ValueTree& clip, juce::Point<float> p) const
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || ! isAudioClip (clip))
            return FadeHandle::none;

        const auto area = clipBounds (clip);

        if (p.y > area.getY() + 11.0f || area.getWidth() < 18.0f)
            return FadeHandle::none;

        const float dIn = std::abs (p.x - fadeHandleBox (fadeInX (*edit, clip), area).getCentreX());
        const float dOut = std::abs (p.x - fadeHandleBox (fadeOutX (*edit, clip), area).getCentreX());

        if (juce::jmin (dIn, dOut) > 6.0f)
            return FadeHandle::none;

        return dOut < dIn ? FadeHandle::out : FadeHandle::in;
    }

    /** The cut the slice tool would make right now (x, top lane, bottom lane). */
    struct SliceLine { bool active = false; double beat = 0.0; int lane1 = 0, lane2 = 0; } slice;

    double sliceBeatAt (const juce::MouseEvent& e) const
    {
        return e.mods.isAltDown() ? beatAtX (e.position.x) : snapNearest (beatAtX (e.position.x));
    }

    /** Cuts every clip on these tracks that the line crosses. Returns how many were cut. */
    int cutAcross (double beat, int lane1, int lane2)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return 0;

        int cuts = 0;

        for (auto& c : getClips (*edit))
        {
            const int lane = c[ids::lane];

            if (lane >= juce::jmin (lane1, lane2) && lane <= juce::jmax (lane1, lane2)
                 && splitClip (*edit, c, beat).isValid())
                ++cuts;
        }

        return cuts;
    }

    void paint (juce::Graphics& g) override
    {
        auto* edit = state.getEdit();
        g.fillAll (theme::colours::lane2);

        if (edit == nullptr)
            return;

        auto visible = g.getClipBounds();
        owner.updateRows();
        const auto audible = getAudibleLanes (*edit);

        // Only the rows on screen (there are 200 tracks).
        const int firstRow = juce::jmax (0, visible.getY() / laneHeight);
        const int lastRow = juce::jmin ((int) owner.rows.size() - 1, visible.getBottom() / laneHeight);

        for (int r = firstRow; r <= lastRow; ++r)
        {
            const auto& row = owner.rows[(size_t) r];
            const int y = r * laneHeight;

            if (row.lane < 0)
            {
                // A group's header row: its colour all along (a folded one shows its clips small).
                const auto c = row.group.colour.isTransparent() ? theme::colours::accent : row.group.colour;
                g.setColour (theme::colours::wellAlt);
                g.fillRect (0, y, getWidth(), laneHeight);
                g.setColour (c.withAlpha (row.folded ? 0.16f : 0.10f));
                g.fillRect (0, y, getWidth(), laneHeight);
                g.setColour (c.withAlpha (0.5f));
                g.fillRect (0, y + laneHeight - 1, getWidth(), 1);
                continue;
            }

            g.setColour (r % 2 == 0 ? theme::colours::lane1 : theme::colours::lane2);
            g.fillRect (0, y, getWidth(), laneHeight);

            // A coloured track (e.g. from an FL template) is tinted all along.
            const auto tint = getLaneColour (*edit, row.lane);

            if (! tint.isTransparent())
            {
                g.setColour (tint.withAlpha (0.07f));
                g.fillRect (0, y, getWidth(), laneHeight);
            }
        }

        // Beat and bar lines
        const int firstBeat = juce::jmax (0, (int) beatAtX ((float) visible.getX()) - 1);
        const int lastBeat = (int) beatAtX ((float) visible.getRight()) + 1;

        const int beatsPerBar = juce::roundToInt (getBeatsPerBar (*edit));

        // Zoomed far out: only every bar (or every few bars), so the lines don't turn into a wall.
        const float barPixels = owner.pixelsPerBeat * (float) beatsPerBar;
        const int barLineEvery = barPixels < 4.0f ? 8 : barPixels < 8.0f ? 4 : barPixels < 14.0f ? 2 : 1;
        const bool beatLines = owner.pixelsPerBeat >= 6.0f;

        for (int b = firstBeat; b <= lastBeat; ++b)
        {
            const bool barLine = b % beatsPerBar == 0;

            if (barLine ? (b / beatsPerBar) % barLineEvery != 0 : ! beatLines)
                continue;

            g.setColour (barLine ? theme::colours::gridBar : theme::colours::grid);
            g.drawVerticalLine ((int) xForBeat (b), 0.0f, (float) getHeight());
        }

        // Tempo changes: a faint amber line down the playlist.
        for (auto& t : getTempoChanges (*edit))
        {
            g.setColour (theme::colours::amber.withAlpha (0.35f));
            g.drawVerticalLine ((int) xForBeat (t.beat), 0.0f, (float) getHeight());
        }

        // Markers: a dashed line down the playlist in the marker's colour.
        for (auto& m : getMarkers (*edit))
        {
            const float x = xForBeat (getMarkerBeat (m));

            if (x < (float) visible.getX() - 2.0f || x > (float) visible.getRight() + 2.0f)
                continue;

            const auto c = getMarkerColour (m);
            g.setColour ((c.isTransparent() ? theme::colours::lilac : c).withAlpha (0.45f));
            const float dashes[] = { 5.0f, 4.0f };
            g.drawDashedLine ({ x + 0.5f, (float) visible.getY(), x + 0.5f, (float) visible.getBottom() }, dashes, 2, 1.0f);
        }

        // Song end
        g.setColour (theme::colours::muted.withAlpha (0.5f));
        g.drawVerticalLine ((int) xForBeat (getSongLengthBeats (*edit)), 0.0f, (float) getHeight());

        // Punch in / out range (recording keeps only this part)
        const auto& punch = state.session.getRecorder().getPunch();

        if (punch.isActive())
        {
            const float x1 = xForBeat (punch.inBeat), x2 = xForBeat (punch.outBeat);
            g.setColour (theme::colours::red.withAlpha (0.07f));
            g.fillRect (x1, 0.0f, x2 - x1, (float) getHeight());
            g.setColour (theme::colours::red.withAlpha (0.45f));
            g.drawVerticalLine ((int) x1, 0.0f, (float) getHeight());
            g.drawVerticalLine ((int) x2, 0.0f, (float) getHeight());
        }

        // Clips
        const auto viewedSid = state.getPattern()[ids::sid].toString();

        for (auto& clip : getClips (*edit))
        {
            auto r = clipBounds (clip).reduced (0.5f, 1.5f);

            if (! r.intersects (visible.toFloat()))
                continue;

            // In a folded group: just a small bar in the clip's colour.
            if (owner.isLaneHidden (clip[ids::lane]))
            {
                const auto p = findPattern (*edit, clip[ids::pattern].toString());
                const auto c = isAudioClip (clip) ? theme::colours::sky
                             : isAutomationClip (clip) ? theme::colours::teal
                             : (p.isValid() ? patternColour (p) : theme::colours::dim);
                g.setColour (c.withAlpha (0.8f));
                g.fillRoundedRectangle (clipBounds (clip).reduced (0.5f, 0.0f), 1.5f);
                continue;
            }

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

            const auto colour = pattern.isValid() ? patternColour (pattern) : theme::colours::dim;
            g.setColour (colour.withAlpha (0.30f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (colour.withAlpha (0.85f));
            g.fillRoundedRectangle (r.withHeight (juce::jmin (r.getHeight(), 14.0f)), 5.0f);   // the name strip
            g.fillRect (r.withHeight (juce::jmin (r.getHeight(), 14.0f)).withTrimmedTop (7.0f));
            g.setColour (pattern[ids::sid].toString() == viewedSid ? theme::colours::text : colour);
            g.drawRoundedRectangle (r, 5.0f, pattern[ids::sid].toString() == viewedSid ? 1.6f : 1.0f);

            if (! pattern.isValid())
                continue;

            // Mini preview, FL-style: one thin row per channel that has notes in this pattern.
            const double clipLen = getClipLength (*edit, clip);
            const double patternOffset = getPatternOffset (clip);
            juce::StringArray rows;

            for (auto* t : getChannels (*edit))
                if (! getNotes (pattern, getChannelID (*t)).isEmpty())
                    rows.add (getChannelID (*t));

            if (! rows.isEmpty())
            {
                const float top = r.getY() + 16.0f, h = r.getHeight() - 18.0f;
                const float rowH = h / (float) rows.size();
                g.setColour (colour.brighter (0.6f));

                for (auto n : pattern)
                {
                    const double b = (double) n[ids::b] - patternOffset;   // within the clip

                    if (! n.hasType (ids::PNOTE) || b < -1.0e-6 || b >= clipLen)
                        continue;

                    const int row = rows.indexOf (n[ids::ch].toString());

                    if (row < 0)
                        continue;

                    const float x = r.getX() + xForBeat (b);
                    const float w = juce::jmax (1.5f, xForBeat (juce::jmin ((double) n[ids::l], clipLen - b)) - 1.0f);
                    g.fillRect (x, top + row * rowH, w, juce::jmax (1.0f, rowH - 1.0f));
                }
            }

            g.setColour (theme::colours::accentText);
            g.setFont (theme::ui (10.5f, theme::Weight::bold));
            g.drawText (pattern[ids::name].toString(), r.reduced (5.0f, 0.0f).withHeight (14.0f).toNearestInt(),
                        juce::Justification::centredLeft, true);
        }

        // Muted tracks and groups: everything on them greyed out (their clips don't play).
        for (int r = firstRow; r <= lastRow; ++r)
        {
            const auto& row = owner.rows[(size_t) r];
            const bool muted = row.lane >= 0 ? ! audible[(size_t) row.lane] : row.group.muted;

            if (muted)
            {
                g.setColour (theme::colours::bg.withAlpha (0.55f));
                g.fillRect (0, r * laneHeight, getWidth(), laneHeight);
            }
        }

        // Where something dragged in (rack or browser) would land
        if (dropPreview.lane >= 0)
        {
            const double length = previewLength (*edit);

            if (length > 0.0)
            {
                const juce::Rectangle<float> r (xForBeat (dropPreview.beat), (float) owner.laneTop (dropPreview.lane) + 1.5f,
                                                xForBeat (dropPreview.beat + length) - xForBeat (dropPreview.beat),
                                                (float) laneHeight - 3.0f);
                g.setColour (theme::colours::accent.withAlpha (0.18f));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (theme::colours::accent);
                g.drawRoundedRectangle (r, 5.0f, 1.5f);
            }
        }

        // The cut the slice tool is about to make
        if (slice.active)
        {
            const float x = xForBeat (slice.beat);
            const float y1 = (float) owner.laneTop (juce::jmin (slice.lane1, slice.lane2));
            const float y2 = (float) (owner.laneTop (juce::jmax (slice.lane1, slice.lane2)) + laneHeight);
            g.setColour (theme::colours::amber);
            const float dashes[] = { 4.0f, 3.0f };
            g.drawDashedLine ({ x, y1, x, y2 }, dashes, 2, 1.5f);
        }

        // Start marker and playhead
        g.setColour (theme::colours::green.withAlpha (0.55f));
        g.drawVerticalLine ((int) xForBeat (state.songStartBeat), 0.0f, (float) getHeight());

        const double playBeat = state.getSongPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (theme::colours::text);
            g.fillRect (juce::Rectangle<float> (xForBeat (playBeat) - 1.0f, 0.0f, 2.0f, (float) getHeight()));
        }
    }

    void paintAutomationClip (juce::Graphics& g, te::Edit& edit, const juce::ValueTree& clip, juce::Rectangle<float> r)
    {
        const juce::Colour colour = theme::colours::teal;
        const bool selected = state.selectedAutomation == clip[ids::sid].toString();
        g.setColour (colour.withAlpha (0.16f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (selected ? theme::colours::text : colour);
        g.drawRoundedRectangle (r, 5.0f, selected ? 1.6f : 1.0f);

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

        g.setColour (theme::colours::text);
        g.setFont (theme::ui (10.0f, theme::Weight::semibold));
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

        // The track's colour if it has one (takes on a "CHORUS" track look like it), else the channel's.
        const auto laneColour = getLaneColour (edit, clip[ids::lane]);
        const auto colour = laneColour.isTransparent() ? EditorState::channelColour (index) : laneColour;
        const bool selected = state.selectedChannel == channelID;

        g.setColour (colour.withAlpha (0.22f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (selected ? theme::colours::text : colour);
        g.drawRoundedRectangle (r, 5.0f, selected ? 1.6f : 1.0f);

        auto& store = state.session.getAssetStore();
        auto body = r.reduced (2.0f, 2.0f);

        if (store.has (hash))
        {
            if (auto* thumb = owner.getThumbnail (hash))
            {
                // Left channel at full clip height: easier to read than two thin strips.
                // The part of the file it plays (speed: more or less of it), as loud as the clip's volume.
                const double offset = clip[ids::offset], duration = clip[ids::duration];
                const float zoom = juce::jlimit (0.05f, 4.0f, juce::Decibels::decibelsToGain ((float) getClipGainDb (clip)));
                g.setColour (colour.brighter (0.4f).withAlpha (0.9f));
                thumb->drawChannel (g, body.toNearestInt(), offset, offset + duration * getClipSpeed (clip), 0, zoom);
            }
        }
        else
        {
            const double progress = store.getProgress (hash);
            g.setColour (theme::colours::muted);
            g.setFont (theme::ui (11.0f));
            g.drawText (progress >= 0.0 ? TRANS ("receiving") + " " + juce::String (juce::roundToInt (progress * 100.0)) + "%"
                                        : TRANS ("waiting for the file..."),
                        body.withTrimmedTop (12.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }

        // Fades: the faded part darkened, with the curve on top.
        auto drawFade = [&] (float from, float to, bool fadeIn)
        {
            if (std::abs (to - from) < 1.0f)
                return;

            juce::Path curve, shade;
            const float top = r.getY() + 1.0f, bottom = r.getBottom() - 1.0f;
            const int steps = juce::jlimit (4, 48, (int) std::abs (to - from) / 3);

            for (int i = 0; i <= steps; ++i)
            {
                const float t = (float) i / (float) steps;
                const float level = fadeIn ? t : 1.0f - t;
                const float shaped = level * level * (3.0f - 2.0f * level);   // the S-curve the engine uses
                const auto point = juce::Point<float> (from + (to - from) * t, bottom - (bottom - top) * shaped);

                if (i == 0) curve.startNewSubPath (point);
                else        curve.lineTo (point);
            }

            shade = curve;
            shade.lineTo (to, top);
            shade.lineTo (from, top);
            shade.closeSubPath();
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillPath (shade);
            g.setColour (theme::colours::text.withAlpha (0.7f));
            g.strokePath (curve, juce::PathStrokeType (1.2f));
        };

        const float inX = juce::jmin (r.getRight(), fadeInX (edit, clip));
        const float outX = juce::jmax (r.getX(), fadeOutX (edit, clip));
        drawFade (r.getX(), inX, true);
        drawFade (outX, r.getRight(), false);

        // The fade handles, while the mouse is over the clip (or a fade is set).
        const bool hovered = hoverClip == clip[ids::sid].toString();

        for (auto [x, set] : { std::pair<float, bool> { inX, getClipFadeIn (clip) > 0.0 },
                               std::pair<float, bool> { outX, getClipFadeOut (clip) > 0.0 } })
        {
            if (! hovered && ! set)
                continue;

            auto box = fadeHandleBox (x, r);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRect (box.expanded (1.0f));
            g.setColour (hovered ? theme::colours::text : theme::colours::muted);
            g.fillRect (box);
        }

        // Name on a small dark tab so it stays readable over the waveform, with the clip's
        // settings when they are changed (volume, pitch, speed).
        auto name = getAssetName (edit, hash);
        juce::StringArray extras;

        if (std::abs (getClipGainDb (clip)) > 0.05)
            extras.add ((getClipGainDb (clip) > 0.0 ? "+" : "") + juce::String (getClipGainDb (clip), 1) + " dB");

        if (std::abs (getClipPitch (clip)) > 0.005)
            extras.add ((getClipPitch (clip) > 0.0 ? "+" : "") + juce::String (getClipPitch (clip), std::abs (std::fmod (getClipPitch (clip), 1.0)) < 0.005 ? 0 : 2) + " st");

        if (std::abs (getClipSpeed (clip) - 1.0) > 1.0e-4)
            extras.add (juce::String (juce::roundToInt (getClipSpeed (clip) * 100.0)) + "%");

        if (! extras.isEmpty())
            name << "  " << extras.joinIntoString ("  ");
        juce::Font font (theme::ui (10.5f, theme::Weight::semibold));
        const float tabWidth = juce::jmin (r.getWidth() - 2.0f, juce::GlyphArrangement::getStringWidth (font, name) + 12.0f);
        auto tab = juce::Rectangle<float> (r.getX() + 1.0f, r.getY() + 1.0f, tabWidth, 13.0f);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (tab, 4.0f);
        g.setColour (theme::colours::text);
        g.setFont (font);
        g.drawText (name, tab.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto c = clipAt (e.position);
        const auto hover = isAudioClip (c) ? c[ids::sid].toString() : juce::String();

        if (hover != hoverClip)
        {
            hoverClip = hover;
            repaint();
        }

        if (owner.sliceTool)
        {
            setMouseCursor (juce::MouseCursor::CrosshairCursor);
            return;
        }

        const bool onEdge = c.isValid() && (fadeHandleAt (c, e.position) != FadeHandle::none
                                             || e.position.x > clipBounds (c).getRight() - 6.0f
                                             || (! isAutomationClip (c) && e.position.x < clipBounds (c).getX() + 6.0f));
        setMouseCursor (onEdge ? juce::MouseCursor::LeftRightResizeCursor
                               : c.isValid() ? juce::MouseCursor::DraggingHandCursor
                                             : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverClip.isNotEmpty())
        {
            hoverClip = {};
            repaint();
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // FL's tool keys (only without Ctrl/Alt, so the app's shortcuts still work).
        if (key.getModifiers().isCommandDown() || key.getModifiers().isAltDown())
            return false;

        if (key.getTextCharacter() == 'p' || key.getTextCharacter() == 'P')   { owner.setSliceTool (false); return true; }
        if (key.getTextCharacter() == 'c' || key.getTextCharacter() == 'C')   { owner.setSliceTool (true); return true; }
        return false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto hit = clipAt (e.position);

        if (isAudioClip (hit) && ! owner.sliceTool)
            owner.showClipProperties (hit[ids::sid].toString(), localAreaToGlobal (clipBounds (hit).toNearestInt()));
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
        grabKeyboardFocus();   // P / C pick the tool

        // A group's header row: nothing goes there (its tracks are below it, or folded away).
        if (! hit.isValid() && isGroupRowAt (e.position.y))
            return;

        if (e.mods.isPopupMenu())
        {
            // On a clip's name strip: its menu (rename, colour, insert...). Elsewhere: delete, like FL.
            if (hit.isValid() && ! isAutomationClip (hit) && e.position.y < clipBounds (hit).getY() + 16.0f)
            {
                showClipMenu (hit, sliceBeatAt (e));
                return;
            }

            if (hit.isValid())
                removeClip (*edit, hit);

            repaint();
            return;
        }

        if (owner.sliceTool)
        {
            // Slice: a cut line from here; let go to cut every clip it crosses.
            slice = { true, sliceBeatAt (e), laneAtY (e.position.y), laneAtY (e.position.y) };
            repaint();
            return;
        }

        if (hit.isValid())
        {
            dragClip = hit;
            const auto bounds = clipBounds (hit);
            const auto fade = fadeHandleAt (hit, e.position);
            dragMode = fade == FadeHandle::in ? DragMode::fadeIn
                     : fade == FadeHandle::out ? DragMode::fadeOut
                     : e.position.x > bounds.getRight() - 6.0f ? (isAudioClip (hit) && e.mods.isAltDown() ? DragMode::stretch : DragMode::resize)
                     : (! isAutomationClip (hit) && e.position.x < bounds.getX() + 6.0f) ? DragMode::trimStart
                     : DragMode::move;
            grabStart = hit[ids::start];
            grabLane = hit[ids::lane];
            grabBeat = beatAtX (e.position.x);
            grabOffset = isAudioClip (hit) ? (double) hit[ids::offset] : getPatternOffset (hit);
            grabDuration = isAudioClip (hit) ? (double) hit[ids::duration] : getClipLength (*edit, hit);
            grabSpeed = getClipSpeed (hit);
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
            grabLane = -1;   // new: its track decides its insert in any case
            grabBeat = beatAtX (e.position.x);
            clickedExisting = false;
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();

        if (slice.active)
        {
            const int lane = laneAtY (e.position.y);

            if (lane != slice.lane2)
            {
                slice.lane2 = lane;
                repaint();
            }

            return;
        }

        if (edit == nullptr || ! dragClip.isValid() || e.getDistanceFromDragStart() < 3)
            return;

        moved = true;
        auto* um = &edit->getUndoManager();

        if (dragMode == DragMode::fadeIn || dragMode == DragMode::fadeOut)
        {
            // Fades follow the mouse freely (no grid), in seconds.
            const double startSeconds = beatsToSeconds (*edit, dragClip[ids::start]);
            const double endSeconds = startSeconds + (double) dragClip[ids::duration];
            const double mouseSeconds = beatsToSeconds (*edit, beatAtX (e.position.x));

            if (dragMode == DragMode::fadeIn)
                setClipFades (*edit, dragClip, mouseSeconds - startSeconds, getClipFadeOut (dragClip));
            else
                setClipFades (*edit, dragClip, getClipFadeIn (dragClip), endSeconds - mouseSeconds);

            repaint();
            return;
        }

        if (dragMode == DragMode::stretch)
        {
            // Alt + right edge: the same part of the file over a new length (time-stretch).
            const double startSeconds = beatsToSeconds (*edit, grabStart);
            const double endBeat = e.mods.isShiftDown() ? beatAtX (e.position.x) : snapNearest (beatAtX (e.position.x));
            const double newDuration = juce::jmax (0.05, beatsToSeconds (*edit, endBeat) - startSeconds);
            const double used = grabDuration * grabSpeed;   // seconds of the file
            const double speed = juce::jlimit (minClipSpeed, maxClipSpeed, used / newDuration);

            if (std::abs (speed - getClipSpeed (dragClip)) > 1.0e-6)
            {
                if (std::abs (speed - 1.0) < 1.0e-6) dragClip.removeProperty (ids::speed, um);
                else                                 dragClip.setProperty (ids::speed, speed, um);

                dragClip.setProperty (ids::duration, used / speed, um);
                setClipFades (*edit, dragClip, getClipFadeIn (dragClip), getClipFadeOut (dragClip));
            }

            repaint();
            return;
        }

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
            // (With a speed change, a second on the timeline is `speed` seconds of the file.)
            const double fileSeconds = juce::jmax (0.05, getAssetSeconds (*edit, dragClip[ids::asset].toString()));
            const double startSeconds = beatsToSeconds (*edit, grabStart);

            if (dragMode == DragMode::resize)
            {
                const double endBeat = snapNearest (beatAtX (e.position.x));
                const double duration = juce::jlimit (0.05, juce::jmax (0.05, (fileSeconds - grabOffset) / grabSpeed),
                                                      beatsToSeconds (*edit, endBeat) - startSeconds);

                if (std::abs (duration - (double) dragClip[ids::duration]) > 1.0e-6)
                    dragClip.setProperty (ids::duration, duration, um);
            }
            else   // trimStart: move the left edge, keeping the right edge where it is
            {
                const double endSeconds = startSeconds + grabDuration;
                double newStartSeconds = beatsToSeconds (*edit, snapNearest (beatAtX (e.position.x)));
                newStartSeconds = juce::jlimit (startSeconds - grabOffset / grabSpeed, endSeconds - 0.05, newStartSeconds);
                newStartSeconds = juce::jmax (0.0, newStartSeconds);

                const double newOffset = grabOffset + (newStartSeconds - startSeconds) * grabSpeed;
                const double newStart = secondsToBeats (*edit, newStartSeconds);

                if (std::abs (newStart - (double) dragClip[ids::start]) > 1.0e-6)
                {
                    dragClip.setProperty (ids::start, newStart, um);
                    dragClip.setProperty (ids::offset, juce::jmax (0.0, newOffset), um);
                    dragClip.setProperty (ids::duration, endSeconds - newStartSeconds, um);
                }
            }
        }
        else if (dragMode == DragMode::trimStart && ! isAutomationClip (dragClip))
        {
            // A pattern clip's left edge: it starts later (or earlier) in its pattern,
            // its right edge stays put. It can't start before the pattern does.
            const double end = grabStart + grabDuration;
            const double newStart = juce::jlimit (juce::jmax (0.0, grabStart - grabOffset), end - stepBeats,
                                                  snapNearest (beatAtX (e.position.x)));

            if (std::abs (newStart - (double) dragClip[ids::start]) > 1.0e-6)
            {
                const double newOffset = juce::jmax (0.0, grabOffset + (newStart - grabStart));
                dragClip.setProperty (ids::start, newStart, um);
                dragClip.setProperty (ids::length, end - newStart, um);

                if (newOffset < 1.0e-6) dragClip.removeProperty (ids::poff, um);
                else                    dragClip.setProperty (ids::poff, newOffset, um);
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
        if (slice.active)
        {
            slice.active = false;

            if (cutAcross (slice.beat, slice.lane1, slice.lane2) > 0 && state.onSelectionChanged)
                state.onSelectionChanged();

            repaint();
            return;
        }

        // A trimmed audio clip keeps its fades inside it.
        if (auto* edit = state.getEdit(); edit != nullptr && dragClip.isValid() && isAudioClip (dragClip) && moved)
            setClipFades (*edit, dragClip, getClipFadeIn (dragClip), getClipFadeOut (dragClip));

        // A take put on (or moved to) a track that sends takes to an insert plays through it.
        if (dragClip.isValid() && isAudioClip (dragClip) && (int) dragClip[ids::lane] != grabLane)
            routeTakeAndTell (state, dragClip);

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
                // A pattern opens in the piano roll, on the instrument you last edited in it.
                state.viewPattern (dragClip[ids::pattern].toString());

                if (state.showPianoRoll)
                    state.showPianoRoll();
            }

            if (state.onSelectionChanged)
                state.onSelectionChanged();
        }

        dragClip = {};
    }

    /** Right-click on a clip's name: pattern (rename, colour, make unique, delete) or
        audio take (insert, rename, delete). */
    void showClipMenu (juce::ValueTree clip, double atBeat)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        juce::PopupMenu m;
        const auto clipSid = clip[ids::sid].toString();

        if (isAudioClip (clip))
        {
            auto* channel = findChannel (*edit, clip[ids::ch].toString());
            const auto hash = clip[ids::asset].toString();
            m.addSectionHeader (getAssetName (*edit, hash));

            if (channel != nullptr)
            {
                // The insert this take plays through (its channel's route).
                juce::PopupMenu inserts;
                inserts.addItem (10, TRANS ("Master"), true, resolveRoute (*edit, *channel) == nullptr);
                auto list = getInserts (*edit);

                for (int i = 0; i < list.size(); ++i)
                    inserts.addItem (100 + i, juce::String (i + 1) + "  " + list[i]->getName(),
                                     canRouteTo (*edit, *channel, list[i]->itemID.toString()),
                                     getRoute (*channel) == list[i]->itemID.toString());

                auto* now = resolveRoute (*edit, *channel);
                m.addSubMenu (TRANS ("Insert") + ": " + (now != nullptr ? now->getName() : TRANS ("Master")), inserts);
            }

            m.addItem (5, TRANS ("Properties (volume, fades, pitch, speed)..."));
            m.addItem (6, TRANS ("Cut here"));
            m.addItem (7, TRANS ("Bounce to audio (apply its fades, volume, pitch and speed)"), owner.bouncer == nullptr || ! owner.bouncer->isRunning());
            m.addItem (2, TRANS ("Rename..."));
            m.addSeparator();
            m.addItem (3, TRANS ("Delete this clip"));
        }
        else
        {
            auto pattern = findPattern (*edit, clip[ids::pattern].toString());

            if (! pattern.isValid())
                return;

            m.addSectionHeader (pattern[ids::name].toString());
            m.addItem (1, TRANS ("Rename..."));
            const auto c = pattern[ids::colour].toString();
            m.addSubMenu (TRANS ("Colour"), colourMenu (200, 299, TRANS ("Default colour"),
                                                        c.isNotEmpty() ? juce::Colour::fromString (c) : juce::Colours::transparentBlack));
            m.addItem (4, TRANS ("Make unique (a copy of the pattern for this clip only)"));
            m.addItem (6, TRANS ("Cut here"));
            m.addItem (7, TRANS ("Bounce to audio (one audio clip per channel)"), owner.bouncer == nullptr || ! owner.bouncer->isRunning());
            m.addSeparator();
            m.addItem (3, TRANS ("Delete this clip"));
        }

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [safe = juce::Component::SafePointer<Lanes> (this), clipSid, atBeat] (int r)
        {
            if (safe == nullptr || r == 0)
                return;

            auto& st = safe->state;
            auto* e = st.getEdit();
            juce::ValueTree target;

            if (e != nullptr)
                for (auto& cl : getClips (*e))
                    if (cl[ids::sid].toString() == clipSid)
                        target = cl;

            if (! target.isValid())
                return;   // deleted meanwhile (by you or someone else)

            auto* um = &e->getUndoManager();
            auto pattern = findPattern (*e, target[ids::pattern].toString());
            auto* channel = findChannel (*e, target[ids::ch].toString());

            if (r == 3)
            {
                st.beginAction();
                removeClip (*e, target);
            }
            else if (r == 5)
            {
                safe->owner.showClipProperties (clipSid, safe->localAreaToGlobal (safe->clipBounds (target).toNearestInt()));
            }
            else if (r == 6)
            {
                st.beginAction();
                splitClip (*e, target, atBeat);
            }
            else if (r == 7)
            {
                safe->owner.bounceClip (clipSid);
                return;
            }
            else if (r == 1 && pattern.isValid())
            {
                askForText (TRANS ("Rename pattern"), TRANS ("New name:"), pattern[ids::name].toString(), [safe, pattern] (juce::String name) mutable
                {
                    if (safe == nullptr || name.isEmpty() || ! pattern.getParent().isValid())
                        return;

                    if (auto* ed = safe->state.getEdit())
                    {
                        safe->state.beginAction();
                        pattern.setProperty (ids::name, name.substring (0, 40), &ed->getUndoManager());
                    }
                });
            }
            else if (r == 2 && isAudioClip (target))
            {
                const auto hash = target[ids::asset].toString();
                askForText (TRANS ("Rename audio"), TRANS ("New name:"), getAssetName (*e, hash).upToLastOccurrenceOf (".", false, false),
                            [safe, hash] (juce::String name)
                {
                    if (safe == nullptr || name.isEmpty())
                        return;

                    if (auto* ed = safe->state.getEdit())
                    {
                        safe->state.beginAction();
                        renameAsset (*ed, hash, name);
                    }
                });
            }
            else if (r == 4 && pattern.isValid())
            {
                st.beginAction();
                auto copy = clonePattern (*e, pattern);

                if (copy.isValid())
                {
                    target.setProperty (ids::pattern, copy[ids::sid].toString(), um);
                    st.viewedPattern = copy[ids::sid].toString();
                }
            }
            else if (r >= 200 && r <= 299 && pattern.isValid())
            {
                st.beginAction();

                if (r == 299)
                    pattern.removeProperty (ids::colour, um);
                else if (r - 200 < (int) std::size (palette))
                    pattern.setProperty (ids::colour, palette[r - 200].colour.toString(), um);
            }
            else if ((r == 10 || r >= 100) && channel != nullptr)
            {
                st.beginAction();
                auto* insert = r == 10 ? nullptr : getInserts (*e)[r - 100];
                setRoute (*e, *channel, insert != nullptr ? insert->itemID.toString() : juce::String());
            }

            if (st.onSelectionChanged)
                st.onSelectionChanged();

            safe->repaint();
        });
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
    enum class DragMode { move, resize, trimStart, fadeIn, fadeOut, stretch } dragMode = DragMode::move;
    double grabStart = 0.0, grabBeat = 0.0, grabOffset = 0.0, grabDuration = 0.0, grabSpeed = 1.0;
    juce::String hoverClip;   // the audio clip under the mouse (its fade handles show)
    int grabLane = -1;
    bool moved = false, clickedExisting = false;
};

//==============================================================================
class Playlist::Ruler  : public juce::Component,
                         public juce::TooltipClient
{
public:
    Ruler (Playlist& p) : owner (p), state (p.state) {}

    static constexpr int barsH = Playlist::rulerBarsHeight;   // below it: the markers' strip

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colours::wellAlt);
        g.setColour (theme::colours::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
        const int scrollX = owner.viewport->getViewPositionX();
        const float ppb = owner.pixelsPerBeat;
        const float barBeats = (float) state.getBeatsPerBar();
        const int firstBar = juce::jmax (0, (int) (scrollX / ppb / barBeats));
        const int lastBar = (int) ((scrollX + getWidth()) / ppb / barBeats) + 1;
        const float barPixels = ppb * barBeats;
        const int labelEvery = barPixels < 5.0f ? 32 : barPixels < 9.0f ? 16 : barPixels < 18.0f ? 8
                             : barPixels < 32.0f ? 4 : barPixels < 56.0f ? 2 : 1;
        const int lineEvery = barPixels < 4.0f ? 8 : barPixels < 8.0f ? 4 : barPixels < 14.0f ? 2 : 1;

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            if (bar % lineEvery != 0 && bar % labelEvery != 0)
                continue;

            const float x = bar * barBeats * ppb - (float) scrollX;
            g.setColour (theme::colours::gridBar);
            g.drawVerticalLine ((int) x, 8.0f, (float) getHeight());

            if (bar % labelEvery == 0)
            {
                g.setFont (theme::mono (10.5f));
                g.setColour (theme::colours::muted);
                g.drawText (juce::String (bar + 1), (int) x + 3, 0, 40, barsH, juce::Justification::centredLeft);
            }
        }

        const float mx = (float) (state.songStartBeat * ppb) - (float) scrollX;
        juce::Path tri;
        tri.addTriangle (mx - 6.0f, 2.0f, mx + 6.0f, 2.0f, mx, (float) barsH - 2.0f);
        g.setColour (theme::colours::green);
        g.fillPath (tri);

        // Punch in / out range: red while on, grey while off.
        const auto& punch = state.session.getRecorder().getPunch();

        if (punch.hasRange())
        {
            const float x1 = (float) (punch.inBeat * ppb) - (float) scrollX, x2 = (float) (punch.outBeat * ppb) - (float) scrollX;
            const auto colour = punch.on ? theme::colours::red : theme::colours::dim;
            const auto band = juce::Rectangle<float> (x1, 1.0f, x2 - x1, 7.0f);
            g.setColour (colour.withAlpha (punch.on ? 0.85f : 0.6f));
            g.fillRoundedRectangle (band, 2.5f);

            if (band.getWidth() > 34.0f)
            {
                g.setColour (theme::colours::accentText);
                g.setFont (theme::ui (8.5f, theme::Weight::bold));
                g.drawText ("PUNCH", band.toNearestInt(), juce::Justification::centred, false);
            }
        }

        // Tempo changes: an amber tag with the new tempo (a slope in front of it = a ramp).
        if (auto* edit = state.getEdit())
        {
            const auto changes = getTempoChanges (*edit);

            for (int i = 0; i < changes.size(); ++i)
            {
                const auto tag = tempoTagBounds (changes[i]);

                if (tag.getRight() < 0.0f || tag.getX() > (float) getWidth())
                    continue;

                // A thin line exactly where it changes (the tag sits beside a bar number).
                const float at = (float) (changes[i].beat * owner.pixelsPerBeat) - (float) owner.viewport->getViewPositionX();
                g.setColour (theme::colours::amber);
                g.fillRect (at, 8.0f, 1.0f, (float) barsH - 8.0f);
                g.setColour (theme::colours::amber.withAlpha (i == draggingTempo ? 1.0f : 0.85f));
                g.fillRoundedRectangle (tag, 3.0f);

                if (changes[i].ramp)
                {
                    juce::Path slope;
                    slope.addTriangle (tag.getX() - 9.0f, tag.getBottom(), tag.getX(), tag.getY(), tag.getX(), tag.getBottom());
                    g.fillPath (slope);
                }

                g.setColour (theme::colours::accentText);
                g.setFont (theme::mono (9.5f, true));
                g.drawText (bpmLabel (changes[i].bpm), tag.toNearestInt(), juce::Justification::centred, false);
            }
        }

        paintMarkers (g);

        const double playBeat = state.getSongPlayheadBeat();

        if (playBeat >= 0.0)
        {
            g.setColour (theme::colours::text);
            g.fillRect ((float) (playBeat * ppb) - (float) scrollX - 1.0f, 0.0f, 2.0f, (float) getHeight());
        }
    }

    //==============================================================================
    // Markers: flags in the strip under the bar numbers.
    static juce::Colour markerColour (const juce::ValueTree& m)
    {
        const auto c = getMarkerColour (m);
        return c.isTransparent() ? theme::colours::lilac : c;
    }

    static juce::String markerLabel (const juce::ValueTree& m)
    {
        const auto t = m[ids::text].toString();
        return t.isNotEmpty() ? t : TRANS ("Marker");
    }

    juce::Rectangle<float> markerBounds (const juce::ValueTree& m) const
    {
        const float x = (float) (getMarkerBeat (m) * owner.pixelsPerBeat) - (float) owner.viewport->getViewPositionX();
        const float w = juce::jmin (180.0f, juce::GlyphArrangement::getStringWidth (theme::ui (10.0f, theme::Weight::semibold), markerLabel (m)) + 12.0f);
        return { x, (float) barsH + 1.0f, w, (float) (getHeight() - barsH) - 3.0f };
    }

    /** The marker under a point (the last drawn wins), or invalid. */
    juce::ValueTree markerAt (juce::Point<float> p) const
    {
        if (p.y < (float) barsH)
            return {};

        if (auto* edit = state.getEdit())
        {
            const auto markers = getMarkers (*edit);

            for (int i = markers.size(); --i >= 0;)
                if (markerBounds (markers[i]).expanded (2.0f, 0.0f).contains (p))
                    return markers[i];
        }

        return {};
    }

    void paintMarkers (juce::Graphics& g)
    {
        g.setColour (theme::colours::well);
        g.fillRect (0, barsH, getWidth(), getHeight() - barsH - 1);

        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        for (auto& m : getMarkers (*edit))
        {
            const auto r = markerBounds (m);

            if (r.getRight() < 0.0f || r.getX() > (float) getWidth())
                continue;

            const auto c = markerColour (m);
            const bool dragged = m[ids::sid].toString() == draggingMarker;
            g.setColour (c.withAlpha (dragged ? 0.5f : 0.28f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (c);
            g.fillRect (r.getX(), r.getY() - 1.0f, 2.0f, r.getHeight() + 2.0f);   // the exact spot
            g.setColour (theme::colours::text);
            g.setFont (theme::ui (10.0f, theme::Weight::semibold));
            g.drawText (markerLabel (m), r.withTrimmedLeft (6.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }
    }

    juce::String getTooltip() override
    {
        const auto p = getMouseXYRelative().toFloat();

        if (auto m = markerAt (p); m.isValid())
        {
            const auto who = m[ids::author].toString();
            return markerLabel (m) + (who.isNotEmpty() ? "  (" + TRANS ("by") + " " + who + ")" : juce::String())
                 + "\n" + TRANS ("Click: play from here. Drag: move. Double-click: edit. Right-click: colour, delete");
        }

        if (p.y >= (float) barsH)
            return TRANS ("Markers: double-click (or right-click) here to add one - everyone sees it");

        return TRANS ("Click: where SONG mode starts. Right-click: tempo change, marker, punch in / out. Ctrl + drag: punch range");
    }

    /** Asks for a marker's text: a new marker at a beat (sid ""), or an existing one. */
    void askMarker (const juce::String& sid, double beat)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const auto existing = findMarker (*edit, sid);
        auto* w = new juce::AlertWindow (existing.isValid() ? TRANS ("Edit marker") : TRANS ("New marker"),
                                         TRANS ("A note everyone sees on the ruler (\"drop\", \"verse 2\", \"fix the hats here\")")
                                           + (existing.isValid() ? "\n" + TRANS ("Empty = delete the marker") : juce::String()),
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("text", existing[ids::text].toString());
        w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Ruler> (this), w, sid, beat] (int r)
        {
            if (safe == nullptr || r != 1)
                return;

            auto& st = safe->state;
            auto* ed = st.getEdit();

            if (ed == nullptr)
                return;

            const auto text = w->getTextEditorContents ("text").trim();
            auto m = findMarker (*ed, sid);
            st.beginAction();

            if (m.isValid())
            {
                if (text.isEmpty())
                    removeMarker (*ed, m);
                else
                    setMarkerText (*ed, m, text);
            }
            else if (sid.isEmpty() && text.isNotEmpty())
            {
                addMarker (*ed, beat, text, st.session.getUserName());
            }

            safe->owner.repaint();
            safe->owner.lanes->repaint();
            safe->repaint();
        }), true);
    }

    void showMarkerMenu (const juce::ValueTree& marker)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const auto sid = marker[ids::sid].toString();
        juce::PopupMenu m;
        m.addSectionHeader (markerLabel (marker));
        m.addItem (1, TRANS ("Play from here"));
        m.addItem (2, TRANS ("Edit text..."));
        m.addSubMenu (TRANS ("Colour"), colourMenu (200, 299, TRANS ("Default colour"), getMarkerColour (marker)));
        m.addSeparator();
        m.addItem (3, TRANS ("Delete marker"));

        if (const auto who = marker[ids::author].toString(); who.isNotEmpty())
            m.addItem (-1, TRANS ("Added by") + " " + who, false);

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [safe = juce::Component::SafePointer<Ruler> (this), sid] (int r)
        {
            if (safe == nullptr || r == 0)
                return;

            auto& st = safe->state;
            auto* e = st.getEdit();
            auto mk = e != nullptr ? findMarker (*e, sid) : juce::ValueTree();

            if (! mk.isValid())
                return;

            if (r == 1) { safe->jumpTo (getMarkerBeat (mk)); return; }
            if (r == 2) { safe->askMarker (sid, 0.0); return; }

            st.beginAction();

            if (r == 3)
                removeMarker (*e, mk);
            else if (r >= 200 && r <= 299)
                setMarkerColour (*e, mk, r == 299 || r - 200 >= (int) std::size (palette) ? juce::Colours::transparentBlack
                                                                                           : palette[r - 200].colour);

            safe->owner.repaint();
            safe->owner.lanes->repaint();
            safe->repaint();
        });
    }

    /** Moves the song start marker there (and the playhead, if the song is playing). */
    void jumpTo (double beat)
    {
        state.songStartBeat = juce::jmax (0.0, beat);

        if (state.session.isSongMode())
            if (auto* edit = state.getEdit())
                if (edit->getTransport().isPlaying())
                    state.session.seek (state.beatsToSeconds (state.songStartBeat));

        owner.repaint();
        owner.lanes->repaint();
        repaint();
    }

    juce::String draggingMarker;
    bool markerMoved = false;

    static juce::String bpmLabel (double bpm)
    {
        return std::abs (bpm - std::round (bpm)) < 0.005 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 1);
    }

    juce::Rectangle<float> tempoTagBounds (const TempoChange& t) const
    {
        float x = (float) (t.beat * owner.pixelsPerBeat) - (float) owner.viewport->getViewPositionX();
        const float w = juce::GlyphArrangement::getStringWidth (theme::mono (9.5f, true), bpmLabel (t.bpm)) + 8.0f;

        // On a bar line: beside the bar's number (which is drawn there).
        const double barBeats = state.getBeatsPerBar();
        const double bar = t.beat / barBeats;

        if (std::abs (bar - std::round (bar)) < 1.0e-6)
            x += juce::GlyphArrangement::getStringWidth (theme::mono (10.5f), juce::String ((int) std::round (bar) + 1)) + 6.0f;

        return { x + 1.0f, 9.0f, w, (float) barsH - 10.0f };
    }

    /** The tempo change whose tag is under this point, or -1. */
    int tempoAt (juce::Point<float> p) const
    {
        if (auto* edit = state.getEdit())
        {
            const auto changes = getTempoChanges (*edit);

            for (int i = changes.size(); --i >= 0;)
                if (tempoTagBounds (changes[i]).expanded (2.0f, 6.0f).contains (p))
                    return i;
        }

        return -1;
    }

    void askTempo (int index, double beat)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const auto changes = getTempoChanges (*edit);
        const bool isNew = ! juce::isPositiveAndBelow (index, changes.size());
        const double current = isNew ? edit->tempoSequence.getTempoAt (te::BeatPosition::fromBeats (beat)).getBpm()
                                     : changes[index].bpm;
        const bool ramp = isNew ? false : changes[index].ramp;
        const double at = isNew ? beat : changes[index].beat;

        askForText (isNew ? TRANS ("Tempo change") : TRANS ("Change this tempo"),
                    TRANS ("New tempo from bar {BAR} (BPM):").replace ("{BAR}", barText (at, state.getBeatsPerBar())),
                    bpmLabel (current),
                    [safe = juce::Component::SafePointer<Ruler> (this), index, isNew, at, ramp] (juce::String text)
        {
            const double bpm = text.replaceCharacter (',', '.').getDoubleValue();

            if (safe == nullptr || bpm < minTempo || bpm > maxTempo)
                return;

            if (auto* ed = safe->state.getEdit())
            {
                safe->state.beginAction();

                if (isNew) addTempoChange (*ed, at, bpm, false);
                else       setTempoChange (*ed, index, at, bpm, ramp);

                safe->owner.repaint();
            }
        });
    }

    void showTempoMenu (int index)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const auto t = getTempoChanges (*edit)[index];
        juce::PopupMenu m;
        m.addSectionHeader (TRANS ("Tempo change: {BPM} BPM at bar {BAR}").replace ("{BPM}", bpmLabel (t.bpm))
                                                                           .replace ("{BAR}", barText (t.beat, state.getBeatsPerBar())));
        m.addItem (1, TRANS ("Change the tempo..."));
        m.addItem (2, TRANS ("Jump to it"), true, ! t.ramp);
        m.addItem (3, TRANS ("Ramp into it (speeds up / slows down gradually from the tempo before)"), true, t.ramp);
        m.addSeparator();
        m.addItem (4, TRANS ("Remove this tempo change"));

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [safe = juce::Component::SafePointer<Ruler> (this), index, t] (int r)
        {
            auto* ed = safe != nullptr ? safe->state.getEdit() : nullptr;

            if (ed == nullptr || r == 0)
                return;

            if (r == 1)
            {
                safe->askTempo (index, t.beat);
                return;
            }

            safe->state.beginAction();

            if (r == 2 || r == 3)  setTempoChange (*ed, index, t.beat, t.bpm, r == 3);
            else if (r == 4)       removeTempoChange (*ed, index);

            safe->owner.repaint();
        });
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.position.y >= (float) barsH)
        {
            auto m = markerAt (e.position);
            askMarker (m[ids::sid].toString(), state.snapPlaylistBeat (beatAt (e.position.x)));
            return;
        }

        const int index = tempoAt (e.position);

        if (index >= 0)
            askTempo (index, 0.0);
    }

    double beatAt (float x) const
    {
        return juce::jmax (0.0, (double) (x + (float) owner.viewport->getViewPositionX()) / owner.pixelsPerBeat);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // The markers' strip
        if (e.position.y >= (float) barsH)
        {
            auto m = markerAt (e.position);
            draggingMarker = {};
            markerMoved = false;

            if (e.mods.isPopupMenu())
            {
                if (m.isValid())
                    showMarkerMenu (m);
                else
                    askMarker ({}, state.snapPlaylistBeat (beatAt (e.position.x)));

                return;
            }

            if (m.isValid())
            {
                draggingMarker = m[ids::sid].toString();
                state.beginAction();
                repaint();
                return;
            }
        }

        const int tempo = tempoAt (e.position);

        if (e.mods.isPopupMenu())
        {
            if (tempo >= 0) showTempoMenu (tempo);
            else            showPunchMenu (beatAt (e.position.x));

            return;
        }

        // Drag a tempo tag to move the change.
        draggingTempo = tempo;

        if (tempo >= 0)
        {
            state.beginAction();
            return;
        }

        // Ctrl + drag: where punch in / out records (like a time selection in FL).
        if (e.mods.isCommandDown())
        {
            punchAnchor = state.snapPlaylistBeat (beatAt (e.position.x));
            setPunchRange (punchAnchor, punchAnchor, true);
            return;
        }

        punchAnchor = -1.0;
        setStart (e.position.x, true);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        if (draggingMarker.isNotEmpty())
        {
            auto* edit = state.getEdit();
            auto m = edit != nullptr ? findMarker (*edit, draggingMarker) : juce::ValueTree();

            if (m.isValid() && (markerMoved || e.getDistanceFromDragStart() >= 3))
            {
                markerMoved = true;
                setMarkerBeat (*edit, m, state.snapPlaylistBeatNearest (beatAt (e.position.x)));
                owner.lanes->repaint();
                repaint();
            }

            return;
        }

        if (draggingTempo >= 0)
        {
            auto* edit = state.getEdit();
            const auto changes = edit != nullptr ? getTempoChanges (*edit) : juce::Array<TempoChange>();

            if (! juce::isPositiveAndBelow (draggingTempo, changes.size()) || e.getDistanceFromDragStart() < 3)
                return;

            auto t = changes[draggingTempo];
            const double beat = juce::jmax (stepBeats, state.snapPlaylistBeatNearest (beatAt (e.position.x)));

            if (std::abs (beat - t.beat) > 1.0e-6)
            {
                setTempoChange (*edit, draggingTempo, beat, t.bpm, t.ramp);

                // It may have moved past another one: find it again.
                const auto now = getTempoChanges (*edit);

                for (int i = 0; i < now.size(); ++i)
                    if (std::abs (now[i].beat - beat) < 1.0e-6)
                        draggingTempo = i;
            }

            owner.repaint();
            return;
        }

        if (punchAnchor >= 0.0)
        {
            const double beat = state.snapPlaylistBeatNearest (beatAt (e.position.x));
            setPunchRange (juce::jmin (punchAnchor, beat), juce::jmax (punchAnchor, beat), true);
            return;
        }

        setStart (e.position.x, false);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (draggingMarker.isNotEmpty())
        {
            // A click (no drag): play from the marker.
            if (! markerMoved)
                if (auto* edit = state.getEdit())
                    if (auto m = findMarker (*edit, draggingMarker); m.isValid())
                        jumpTo (getMarkerBeat (m));

            draggingMarker = {};
            repaint();
            return;
        }

        if (draggingTempo >= 0)
        {
            draggingTempo = -1;
            repaint();
            return;
        }

        if (punchAnchor >= 0.0)
        {
            punchAnchor = -1.0;
            auto& rec = state.session.getRecorder();
            auto p = rec.getPunch();

            if (! p.hasRange())
            {
                p.on = false;   // just a click: nothing to punch
                rec.setPunch (p);
            }
            else
            {
                state.session.logMessage (TRANS ("Punch in / out: bars {FROM} to {TO}. Rec plays from a bar before and keeps only this part")
                                            .replace ("{FROM}", barText (p.inBeat, state.getBeatsPerBar()))
                                            .replace ("{TO}", barText (p.outBeat, state.getBeatsPerBar())));
            }

            owner.repaint();
            owner.lanes->repaint();
            notifyPunchChanged();
        }
    }

    static juce::String barText (double beat, double beatsPerBar)
    {
        const double bar = beat / beatsPerBar + 1.0;
        return std::abs (bar - std::round (bar)) < 0.001 ? juce::String ((int) std::round (bar)) : juce::String (bar, 2);
    }

    void setPunchRange (double in, double out, bool on)
    {
        auto& rec = state.session.getRecorder();
        auto p = rec.getPunch();
        p.inBeat = in;
        p.outBeat = out;
        p.on = on;
        rec.setPunch (p);
        repaint();
        owner.lanes->repaint();
    }

    void notifyPunchChanged()
    {
        if (owner.onPunchChanged)
            owner.onPunchChanged();
    }

    void showPunchMenu (double beat)
    {
        auto& rec = state.session.getRecorder();
        const auto p = rec.getPunch();
        const double barBeats = state.getBeatsPerBar();
        const double barStart = std::floor (beat / barBeats + 1.0e-9) * barBeats;
        const double tempoBeat = juce::jmax (stepBeats, state.snapPlaylistBeat (beat));

        juce::PopupMenu m;
        m.addItem (30, TRANS ("Tempo change here (bar {BAR})...").replace ("{BAR}", barText (tempoBeat, barBeats)));
        m.addItem (31, TRANS ("Add a marker here..."));
        m.addSeparator();
        m.addSectionHeader (TRANS ("Punch in / out (recording keeps only this part)"));

        for (int bars : { 1, 2, 4, 8 })
            m.addItem (bars, TRANS ("From bar {BAR}, {N} bar(s)").replace ("{BAR}", barText (barStart, barBeats)).replace ("{N}", juce::String (bars)));

        m.addSeparator();
        m.addItem (20, TRANS ("Punch in / out on"), p.hasRange(), p.isActive());
        m.addItem (21, TRANS ("Clear the punch range"), p.hasRange());
        m.addSeparator();
        m.addItem (-1, TRANS ("Tip: Ctrl + drag here draws the range"), false);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                         [safe = juce::Component::SafePointer<Ruler> (this), barStart, barBeats, tempoBeat] (int r)
        {
            if (safe == nullptr || r == 0)
                return;

            if (r == 30)
            {
                safe->askTempo (-1, tempoBeat);
                return;
            }

            if (r == 31)
            {
                safe->askMarker ({}, tempoBeat);
                return;
            }

            auto& recorder = safe->state.session.getRecorder();
            auto punch = recorder.getPunch();

            if (r >= 1 && r <= 8)
                safe->setPunchRange (barStart, barStart + r * barBeats, true);
            else if (r == 20)
                { punch.on = ! punch.on; recorder.setPunch (punch); }
            else if (r == 21)
                { punch = {}; punch.preRollBars = recorder.getPunch().preRollBars; recorder.setPunch (punch); }

            safe->owner.repaint();
            safe->owner.lanes->repaint();
            safe->notifyPunchChanged();
        });
    }

    double punchAnchor = -1.0;
    int draggingTempo = -1;

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
class Playlist::LaneHeader  : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    LaneHeader (Playlist& p) : owner (p)
    {
        setTooltip (TRANS ("Double-click a track to name it. Right-click: name, colour, mute, and the insert that takes dropped on it play through. "
                           "Drag over several tracks, then right-click: group them (everyone sees all of this)"));
    }

    int rowAt (int y) const      { return owner.rowAtY (y + owner.viewport->getViewPositionY()); }
    int laneAt (int y) const     { return owner.laneAtRowY (y + owner.viewport->getViewPositionY()); }

    const Row* rowPtr (int y) const
    {
        const int r = rowAt (y);
        return juce::isPositiveAndBelow (r, (int) owner.rows.size()) ? &owner.rows[(size_t) r] : nullptr;
    }

    static juce::Colour groupColour (const song::LaneGroup& g)   { return g.colour.isTransparent() ? theme::colours::accent : g.colour; }

    // The fold arrow and the mute button of a group's header row (x positions).
    static constexpr int arrowRight = 18, muteSize = 16;
    juce::Rectangle<int> muteBox (int y) const   { return { getWidth() - muteSize - 5, y + (laneHeight - muteSize) / 2, muteSize, muteSize }; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colours::panel);
        const int scrollY = owner.viewport->getViewPositionY();
        auto* edit = owner.state.getEdit();
        owner.updateRows();
        const auto audible = edit != nullptr ? getAudibleLanes (*edit) : std::vector<bool> ((size_t) numLanes, true);

        const int firstRow = juce::jmax (0, scrollY / laneHeight);
        const int lastRow = juce::jmin ((int) owner.rows.size() - 1, (scrollY + getHeight()) / laneHeight);

        for (int r = firstRow; r <= lastRow; ++r)
        {
            const auto& row = owner.rows[(size_t) r];
            const int y = r * laneHeight - scrollY;

            if (row.lane < 0)
            {
                paintGroupRow (g, row, y);
                continue;
            }

            const int lane = row.lane;
            const bool grouped = row.group.isValid();
            const int indent = grouped ? 6 : 0;   // a grouped track sits right of its group's bracket

            g.setColour (r % 2 == 0 ? theme::colours::lane1 : theme::colours::lane2);
            g.fillRect (0, y, getWidth() - 1, laneHeight);

            if (selectionContains (lane))
            {
                g.setColour (theme::colours::accentSoft);
                g.fillRect (0, y, getWidth() - 1, laneHeight);
            }

            if (grouped)
            {
                g.setColour (groupColour (row.group).withAlpha (0.8f));
                g.fillRect (0, y, 3, laneHeight - (lane == row.group.last() ? 4 : 0));
            }

            if (edit != nullptr)
            {
                // The track's colour: a strip on the left and a light tint.
                const auto colour = getLaneColour (*edit, lane);

                if (! colour.isTransparent())
                {
                    g.setColour (colour.withAlpha (0.14f));
                    g.fillRect (indent, y, getWidth() - 1 - indent, laneHeight);
                    g.setColour (colour);
                    g.fillRect (indent, y + 1, 4, laneHeight - 2);
                }

                // Where takes dropped here play: the insert's number (M = master).
                const auto target = getLaneTarget (*edit, lane);

                if (target.routes)
                {
                    auto* ins = findInsert (*edit, target.insertID);
                    const auto badge = ins != nullptr ? juce::String (getInsertNumber (*edit, *ins)) : juce::String ("M");
                    auto box = juce::Rectangle<float> ((float) getWidth() - 25.0f, (float) y + 3.0f, 21.0f, 11.0f);
                    g.setColour (theme::colours::accentSoft);
                    g.fillRoundedRectangle (box, 3.0f);
                    g.setColour (theme::colours::lilac);
                    g.setFont (theme::mono (8.5f, true));
                    g.drawText (juce::String::fromUTF8 ("\xe2\x86\x92") + badge, box.toNearestInt(), juce::Justification::centred, false);
                }

                // Muted (its clips don't play): a small "M" in the corner.
                if (isLaneMuted (*edit, lane))
                {
                    auto box = juce::Rectangle<float> ((float) getWidth() - 15.0f, (float) y + laneHeight - 13.0f, 11.0f, 10.0f);
                    g.setColour (theme::colours::red.withAlpha (0.85f));
                    g.fillRoundedRectangle (box, 2.5f);
                    g.setColour (theme::colours::accentText);
                    g.setFont (theme::mono (8.0f, true));
                    g.drawText ("M", box.toNearestInt(), juce::Justification::centred, false);
                }
            }

            const auto name = edit != nullptr ? getLaneName (*edit, lane) : juce::String();
            const float alpha = audible[(size_t) lane] ? 1.0f : 0.45f;

            if (name.isEmpty())
            {
                g.setFont (theme::ui (11.0f, theme::Weight::semibold));
                g.setColour (theme::colours::muted.withMultipliedAlpha (alpha));
                g.drawText (TRANS ("Track") + " " + juce::String (lane + 1), 10 + indent, y, getWidth() - 12 - indent, laneHeight, juce::Justification::centredLeft);
            }
            else
            {
                // The track's name, its number small above it.
                g.setFont (theme::mono (9.0f));
                g.setColour (theme::colours::dim.withMultipliedAlpha (alpha));
                g.drawText (juce::String (lane + 1), 8 + indent, y + 2, getWidth() - 10 - indent, 10, juce::Justification::centredLeft);
                g.setFont (theme::ui (11.0f, theme::Weight::semibold));
                g.setColour (theme::colours::text.withMultipliedAlpha (alpha));
                g.drawFittedText (name, 8 + indent, y + 11, getWidth() - 10 - indent, laneHeight - 13, juce::Justification::centredLeft, 1, 0.7f);
            }
        }

        g.setColour (theme::colours::line);
        g.fillRect (getWidth() - 1, 0, 1, getHeight());
    }

    void paintGroupRow (juce::Graphics& g, const Row& row, int y)
    {
        const auto c = groupColour (row.group);
        g.setColour (theme::colours::wellAlt);
        g.fillRect (0, y, getWidth() - 1, laneHeight);
        g.setColour (c.withAlpha (0.22f));
        g.fillRect (0, y, getWidth() - 1, laneHeight);
        g.setColour (c);
        g.fillRect (0, y + (row.folded ? 0 : 4), 3, laneHeight - (row.folded ? 0 : 4));
        g.fillRect (0, y + laneHeight - 1, getWidth() - 1, 1);

        // Fold arrow
        juce::Path arrow;
        const float cx = 11.0f, cy = (float) y + laneHeight * 0.5f;

        if (row.folded)
            arrow.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 4.0f, cy);
        else
            arrow.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);

        g.setColour (theme::colours::text);
        g.fillPath (arrow);

        // Name and how many tracks
        const auto name = row.group.name.isNotEmpty() ? row.group.name : TRANS ("Group");
        const float alpha = row.group.muted ? 0.5f : 1.0f;
        g.setFont (theme::ui (11.0f, theme::Weight::bold));
        g.setColour (theme::colours::text.withMultipliedAlpha (alpha));
        g.drawFittedText (name, arrowRight, y + 2, getWidth() - arrowRight - muteSize - 8, 15, juce::Justification::centredLeft, 1, 0.7f);
        g.setFont (theme::mono (8.5f));
        g.setColour (theme::colours::muted.withMultipliedAlpha (alpha));
        g.drawText (juce::String (row.group.first + 1) + "-" + juce::String (row.group.last() + 1),
                    arrowRight, y + 16, getWidth() - arrowRight - muteSize - 8, 11, juce::Justification::centredLeft, false);

        // Mute button for the whole group
        const auto box = muteBox (y).toFloat();
        g.setColour (row.group.muted ? theme::colours::red : theme::colours::well);
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (row.group.muted ? theme::colours::accentText : theme::colours::muted);
        g.setFont (theme::mono (9.0f, true));
        g.drawText ("M", box.toNearestInt(), juce::Justification::centred, false);
    }

    //==============================================================================
    // Selecting tracks (drag over their names) to group them; this PC only.
    int selFirst = -1, selLast = -1;

    bool selectionContains (int lane) const
    {
        return selFirst >= 0 && lane >= juce::jmin (selFirst, selLast) && lane <= juce::jmax (selFirst, selLast);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto* row = rowPtr (e.y);

        if (row == nullptr)
            return;

        if (row->lane < 0)
        {
            const int y = rowAt (e.y) * laneHeight - owner.viewport->getViewPositionY();

            if (e.mods.isPopupMenu())
                showGroupMenu (row->group.sid);
            else if (muteBox (y).expanded (2).contains (e.getPosition()))
                toggleGroupMute (row->group.sid);
            else
                owner.setGroupFolded (row->group.sid, ! row->folded);

            return;
        }

        if (e.mods.isPopupMenu())
        {
            showLaneMenu (row->lane);
            return;
        }

        if (e.mods.isShiftDown() && selFirst >= 0)
            selLast = row->lane;
        else
            selFirst = selLast = row->lane;

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || selFirst < 0)
            return;

        if (const auto* row = rowPtr (e.y); row != nullptr && row->lane >= 0 && row->lane != selLast)
        {
            selLast = row->lane;
            repaint();
        }
    }

    void toggleGroupMute (const juce::String& sid)
    {
        if (auto* e = owner.state.getEdit())
        {
            const auto g = getLaneGroup (*e, sid);

            if (g.isValid())
            {
                owner.state.beginAction();
                setLaneGroupMuted (*e, sid, ! g.muted);
                owner.refresh();
            }
        }
    }

    void showGroupMenu (const juce::String& sid)
    {
        auto* edit = owner.state.getEdit();

        if (edit == nullptr)
            return;

        const auto group = getLaneGroup (*edit, sid);

        if (! group.isValid())
            return;

        const bool folded = owner.state.foldedGroups.count (sid) > 0;
        juce::PopupMenu m;
        m.addSectionHeader (TRANS ("Group") + ": " + (group.name.isNotEmpty() ? group.name : TRANS ("Group"))
                              + " (" + TRANS ("tracks") + " " + juce::String (group.first + 1) + "-" + juce::String (group.last() + 1) + ")");
        m.addItem (1, TRANS ("Rename..."));
        m.addSubMenu (TRANS ("Colour"), colourMenu (200, 299, TRANS ("Default colour"), group.colour));
        m.addItem (2, TRANS ("Mute the whole group"), true, group.muted);
        m.addItem (3, folded ? TRANS ("Unfold (show its tracks)") : TRANS ("Fold (hide its tracks)"));
        m.addSeparator();
        m.addItem (4, TRANS ("Add the track below to the group"), canGroupLanes (*edit, group.first, group.count + 1, sid));
        m.addItem (5, TRANS ("Take the last track out of the group"), group.count > 1);
        m.addSeparator();
        m.addItem (6, TRANS ("Ungroup (the tracks and their clips stay)"));

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [safe = juce::Component::SafePointer<LaneHeader> (this), sid, group, folded] (int r)
        {
            if (safe == nullptr || r == 0)
                return;

            auto& st = safe->owner.state;
            auto* e = st.getEdit();

            if (e == nullptr || ! getLaneGroup (*e, sid).isValid())
                return;

            if (r == 1)
            {
                safe->askGroupName (sid, group.first, group.count);
                return;
            }

            if (r == 3)
            {
                safe->owner.setGroupFolded (sid, ! folded);
                return;
            }

            st.beginAction();

            if (r >= 200 && r <= 299)
                setLaneGroupColour (*e, sid, r == 299 || r - 200 >= (int) std::size (palette) ? juce::Colours::transparentBlack
                                                                                               : palette[r - 200].colour);
            else if (r == 2) setLaneGroupMuted (*e, sid, ! group.muted);
            else if (r == 4) setLaneGroupRange (*e, sid, group.first, group.count + 1);
            else if (r == 5) setLaneGroupRange (*e, sid, group.first, group.count - 1);
            else if (r == 6)
            {
                st.foldedGroups.erase (sid);
                removeLaneGroup (*e, sid);
            }

            safe->owner.refresh();
        });
    }

    /** Names a new group (sid "") made of these tracks, or renames one. */
    void askGroupName (const juce::String& sid, int first, int count)
    {
        auto* edit = owner.state.getEdit();

        if (edit == nullptr)
            return;

        const auto current = sid.isNotEmpty() ? getLaneGroup (*edit, sid).name : juce::String();
        auto* w = new juce::AlertWindow (sid.isEmpty() ? TRANS ("Group tracks") : TRANS ("Rename the group"),
                                         TRANS ("Tracks {A} to {B}. Name of the group:").replace ("{A}", juce::String (first + 1))
                                                                                         .replace ("{B}", juce::String (first + count)),
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", current.isNotEmpty() ? current : (sid.isEmpty() ? TRANS ("Group") : juce::String()));
        w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<LaneHeader> (this), w, sid, first, count] (int r)
        {
            if (safe == nullptr || r != 1)
                return;

            auto& st = safe->owner.state;
            auto* ed = st.getEdit();

            if (ed == nullptr)
                return;

            st.beginAction();
            const auto name = w->getTextEditorContents ("name");

            if (sid.isEmpty())
            {
                // The group takes the first track's colour, if it has one.
                addLaneGroup (*ed, first, count, name, getLaneColour (*ed, first));
                safe->selFirst = safe->selLast = -1;
            }
            else
            {
                setLaneGroupName (*ed, sid, name);
            }

            safe->owner.refresh();
        }), true);
    }

    void showLaneMenu (int lane)
    {
        auto* edit = owner.state.getEdit();

        if (edit == nullptr || ! juce::isPositiveAndBelow (lane, numLanes))
            return;

        // The tracks to group: the dragged-over ones (if this is one of them), else just this one.
        const bool inSelection = selectionContains (lane);
        const int first = inSelection ? juce::jmin (selFirst, selLast) : lane;
        const int count = inSelection ? std::abs (selLast - selFirst) + 1 : 1;
        const bool canGroup = canGroupLanes (*edit, first, count);

        const auto name = getLaneName (*edit, lane);
        const auto setting = getLaneInsertSetting (*edit, lane);
        juce::PopupMenu m, takes;
        m.addSectionHeader (TRANS ("Track") + " " + juce::String (lane + 1) + (name.isNotEmpty() ? ": " + name : juce::String()));
        m.addItem (1, TRANS ("Rename..."));
        m.addSubMenu (TRANS ("Colour"), colourMenu (200, 299, TRANS ("No colour"), getLaneColour (*edit, lane)));
        m.addItem (2, TRANS ("Mute this track"), true, isLaneMuted (*edit, lane));

        // Where takes (audio clips) dropped on this track play.
        auto* named = findInsertNamed (*edit, name);
        takes.addItem (300, TRANS ("Automatic: the insert with the track's name") + " ("
                              + (named != nullptr ? named->getName() : TRANS ("none")) + ")", true, setting.isEmpty());
        takes.addItem (301, TRANS ("Leave their insert as it is"), true, setting == laneKeepsInsert);
        takes.addSeparator();
        takes.addItem (302, TRANS ("Master"), true, setting == laneToMaster);
        auto inserts = getInserts (*edit);

        for (int i = 0; i < inserts.size(); ++i)
            takes.addItem (400 + i, juce::String (i + 1) + "  " + inserts[i]->getName(), true, setting == inserts[i]->itemID.toString());

        m.addSubMenu (TRANS ("Takes dropped here play through"), takes);
        m.addSeparator();

        if (count > 1)
            m.addItem (3, TRANS ("Group tracks {A} to {B}...").replace ("{A}", juce::String (first + 1)).replace ("{B}", juce::String (first + count)), canGroup);
        else
            m.addItem (3, TRANS ("Make a group of this track..."), canGroup);

        if (! canGroup && getLaneGroupOf (*edit, lane).isValid())
            m.addItem (-1, TRANS ("(already in a group: right-click the group's row)"), false);
        else if (count == 1)
            m.addItem (-1, TRANS ("Tip: drag over several track names first to group them"), false);

        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [safe = juce::Component::SafePointer<LaneHeader> (this), lane, first, count] (int r)
        {
            if (safe == nullptr || r == 0)
                return;

            auto& st = safe->owner.state;
            auto* e = st.getEdit();

            if (e == nullptr)
                return;

            if (r == 1)
            {
                safe->askName (lane);
                return;
            }

            if (r == 3)
            {
                safe->askGroupName ({}, first, count);
                return;
            }

            st.beginAction();

            if (r >= 200 && r <= 299)
                setLaneColour (*e, lane, r == 299 || r - 200 >= (int) std::size (palette) ? juce::Colours::transparentBlack
                                                                                          : palette[r - 200].colour);
            else if (r == 2)    setLaneMuted (*e, lane, ! isLaneMuted (*e, lane));
            else if (r == 300)  setLaneInsertSetting (*e, lane, {});
            else if (r == 301)  setLaneInsertSetting (*e, lane, laneKeepsInsert);
            else if (r == 302)  setLaneInsertSetting (*e, lane, laneToMaster);
            else if (auto* ins = getInserts (*e)[r - 400])
                setLaneInsertSetting (*e, lane, ins->itemID.toString());

            safe->owner.repaint();
            safe->owner.lanes->repaint();
            safe->repaint();
        });
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        if (const auto* row = rowPtr (e.y); row != nullptr && row->lane < 0)
            return;   // (the clicks fold and unfold it)

        askName (laneAt (e.y));
    }

    void askName (int lane)
    {
        auto* edit = owner.state.getEdit();

        if (edit == nullptr || ! juce::isPositiveAndBelow (lane, numLanes))
            return;

        auto* w = new juce::AlertWindow (TRANS ("Name this track"),
                                         TRANS ("Track {N} (empty = no name):").replace ("{N}", juce::String (lane + 1)),
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", getLaneName (*edit, lane));
        w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<LaneHeader> (this), w, lane] (int r)
        {
            if (safe == nullptr || r != 1)
                return;

            if (auto* ed = safe->owner.state.getEdit())
                setLaneName (*ed, lane, w->getTextEditorContents ("name"));

            safe->repaint();
        }), true);
    }

private:
    Playlist& owner;
};

//==============================================================================
Playlist::Playlist (EditorState& s) : state (s)
{
    updateRows();
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

    title.setColour (juce::Label::textColourId, theme::colours::muted);
    title.setFont (theme::ui (11.0f));
    title.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (title);

    formats.registerBasicFormats();

    importButton.setTooltip (TRANS ("Add audio files (WAV, MP3, FLAC...) to the song. You can also drag them onto a lane"));
    importButton.onClick = [this] { showImportDialog(); };
    addAndMakeVisible (importButton);

    automateButton.setTooltip (TRANS ("Make an automation clip for any knob: the one you moved last, or pick one (channels, mixer, plugins). Automation plays in SONG mode"));
    automateButton.onClick = [this] { showAutomateMenu(); };
    addAndMakeVisible (automateButton);

    snapBox.addItem (TRANS ("Snap: bar"), 1);
    snapBox.addItem (TRANS ("Snap: beat"), 2);
    snapBox.addItem (TRANS ("Snap: step"), 3);
    snapBox.addItem (TRANS ("Snap: none"), 4);
    snapBox.setSelectedId (1, juce::dontSendNotification);
    snapBox.setTooltip (TRANS ("Grid that clips snap to. Hold Alt while dragging to move freely"));
    snapBox.onChange = [this]
    {
        const int id = snapBox.getSelectedId();
        state.playlistSnapBeats = id == 1 ? EditorState::snapBar : id == 2 ? 1.0 : id == 3 ? stepBeats : 0.0;
    };
    addAndMakeVisible (snapBox);

    fullScreenButton.getProperties().set ("iconOnly", true);
    fullScreenButton.onClick = [this] { if (onFullScreenToggle) onFullScreenToggle(); };
    addAndMakeVisible (fullScreenButton);
    setFullScreenState (false);

    // Tools, like FL's playlist: Draw (place, move, resize) and Slice (cut clips in two).
    drawToolButton.setTooltip (TRANS ("Draw (P): click to place, drag clips to move them, drag their edges to resize. "
                                      "Audio clips: drag the squares at their top corners to fade, Alt + drag the right edge to stretch"));
    sliceToolButton.setTooltip (TRANS ("Slice (C): click a clip to cut it in two there, or drag down across several tracks "
                                       "to cut them all. Alt: no snap"));
    theme::setIcon (drawToolButton, theme::Icon::pencil);
    theme::setIcon (sliceToolButton, theme::Icon::slice);

    for (auto* b : { &drawToolButton, &sliceToolButton })
    {
        b->getProperties().set ("iconOnly", true);
        theme::setStyle (*b, "toggle");
        b->setClickingTogglesState (false);
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    drawToolButton.onClick = [this] { setSliceTool (false); };
    sliceToolButton.onClick = [this] { setSliceTool (true); };
    lanes->setWantsKeyboardFocus (true);
    setSliceTool (false);

    startTimerHz (30);
}

void Playlist::setSliceTool (bool shouldSlice)
{
    sliceTool = shouldSlice;
    drawToolButton.setToggleState (! sliceTool, juce::dontSendNotification);
    sliceToolButton.setToggleState (sliceTool, juce::dontSendNotification);
    lanes->setMouseCursor (sliceTool ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
}

void Playlist::setFullScreenState (bool isFullScreen)
{
    theme::setIcon (fullScreenButton, isFullScreen ? theme::Icon::exitFullScreen : theme::Icon::fullScreen);
    fullScreenButton.setTooltip (isFullScreen ? TRANS ("Back to the normal layout (Esc)")
                                              : TRANS ("Make the playlist fill the window (Esc goes back)"));
    fullScreenButton.repaint();
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

        mixer.addSubMenu (TRANS ("Master"), master);
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
        menu.addItem (-1, TRANS ("(move a knob, then come back here to automate it)"), false);

    menu.addSeparator();
    menu.addSubMenu (TRANS ("Channels"), channels);
    menu.addSubMenu (TRANS ("Mixer"), mixer);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (automateButton), [this, targets] (int r)
    {
        if (r >= 1 && r <= (int) targets.size() && state.createAutomation)
            state.createAutomation (targets[(size_t) r - 1].first, targets[(size_t) r - 1].second);
    });
}

//==============================================================================
/** An audio clip's settings, like FL's audio clip properties and Pro Tools' clip gain:
    volume, fades, pitch, speed (time-stretch), shown in a bubble next to the clip.
    Every change is shared and undoable (one step per drag). */
class ClipPropertiesPanel  : public juce::Component
{
public:
    ClipPropertiesPanel (EditorState& s, const juce::String& sid) : state (s), clipSid (sid)
    {
        auto clip = findClip();
        auto* edit = state.getEdit();

        if (! clip.isValid() || edit == nullptr)
            return;

        title = getAssetName (*edit, clip[ids::asset].toString());

        const double duration = clip[ids::duration];
        setUp (volume, TRANS ("Volume"), minClipGainDb, maxClipGainDb, 0.1, " dB", 0.0);
        setUp (fadeIn, TRANS ("Fade in"), 0.0, juce::jmax (1.0, duration * 1000.0), 1.0, " ms", 0.0);
        setUp (fadeOut, TRANS ("Fade out"), 0.0, juce::jmax (1.0, duration * 1000.0), 1.0, " ms", 0.0);
        setUp (pitch, TRANS ("Pitch"), -maxClipPitch, maxClipPitch, 1.0, " st", 0.0);
        setUp (fine, TRANS ("Fine"), -100.0, 100.0, 1.0, " ct", 0.0);
        setUp (speed, TRANS ("Speed"), minClipSpeed * 100.0, maxClipSpeed * 100.0, 1.0, " %", 100.0);
        speed.slider.setSkewFactorFromMidPoint (100.0);
        fadeIn.slider.setSkewFactorFromMidPoint (juce::jmin (500.0, duration * 250.0 + 1.0));
        fadeOut.slider.setSkewFactorFromMidPoint (juce::jmin (500.0, duration * 250.0 + 1.0));

        keepPitch.setButtonText (TRANS ("Keep the pitch when changing the speed"));
        keepPitch.setTooltip (TRANS ("On: faster or slower without sounding higher or lower (time-stretch). "
                                     "Off: like a record, faster also sounds higher"));
        keepPitch.onClick = [this]
        {
            if (auto c = findClip(); c.isValid())
                if (auto* e = state.getEdit())
                {
                    state.beginAction();

                    if (keepPitch.getToggleState()) c.removeProperty (ids::smode, &e->getUndoManager());
                    else                            c.setProperty (ids::smode, "resample", &e->getUndoManager());
                }
        };
        addAndMakeVisible (keepPitch);

        fitLabel.setText (TRANS ("Its tempo (BPM):"), juce::dontSendNotification);
        fitLabel.setColour (juce::Label::textColourId, theme::colours::muted);
        fitLabel.setFont (theme::ui (11.5f));
        addAndMakeVisible (fitLabel);
        bpmField.setInputRestrictions (6, "0123456789.,");
        bpmField.setTextToShowWhenEmpty ("90", theme::colours::dim);
        bpmField.setJustification (juce::Justification::centred);
        bpmField.onReturnKey = [this] { fitToTempo(); };
        addAndMakeVisible (bpmField);
        fitButton.setButtonText (TRANS ("Fit to the song"));
        fitButton.setTooltip (TRANS ("For a loop or beat made at another tempo: type its tempo, and the clip is "
                                     "sped up or slowed down to play in time with the song"));
        fitButton.onClick = [this] { fitToTempo(); };
        addAndMakeVisible (fitButton);

        resetButton.setButtonText (TRANS ("Reset"));
        resetButton.setTooltip (TRANS ("Volume 0 dB, no fades, original pitch and speed"));
        resetButton.onClick = [this] { resetAll(); };
        addAndMakeVisible (resetButton);

        readValues();
        setSize (340, 316);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::colours::text);
        g.setFont (theme::ui (13.0f, theme::Weight::bold));
        g.drawText (title, getLocalBounds().reduced (14, 0).removeFromTop (30), juce::Justification::centredLeft, true);
        g.setColour (theme::colours::line);
        g.fillRect (14, 30, getWidth() - 28, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 0);
        r.removeFromTop (38);

        for (auto* row : { &volume, &fadeIn, &fadeOut, &pitch, &fine, &speed })
        {
            auto line = r.removeFromTop (28);
            row->label.setBounds (line.removeFromLeft (70));
            row->slider.setBounds (line);
            r.removeFromTop (2);
        }

        keepPitch.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        auto fit = r.removeFromTop (26);
        fitLabel.setBounds (fit.removeFromLeft (104));
        bpmField.setBounds (fit.removeFromLeft (54));
        fit.removeFromLeft (6);
        fitButton.setBounds (fit);
        r.removeFromTop (8);
        resetButton.setBounds (r.removeFromTop (26).removeFromRight (90));
    }

private:
    struct Row
    {
        juce::Label label;
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    };

    EditorState& state;
    juce::String clipSid, title;
    Row volume, fadeIn, fadeOut, pitch, fine, speed;
    juce::ToggleButton keepPitch;
    juce::Label fitLabel;
    juce::TextEditor bpmField;
    juce::TextButton fitButton, resetButton;
    bool reading = false;

    juce::ValueTree findClip() const
    {
        if (auto* edit = state.getEdit())
            for (auto& c : getClips (*edit))
                if (c[ids::sid].toString() == clipSid)
                    return c;

        return {};
    }

    void setUp (Row& row, const juce::String& name, double min, double max, double step, const juce::String& suffix, double reset)
    {
        row.label.setText (name, juce::dontSendNotification);
        row.label.setColour (juce::Label::textColourId, theme::colours::muted);
        row.label.setFont (theme::ui (12.0f));
        row.slider.setRange (min, max, step);
        row.slider.setTextValueSuffix (suffix);
        row.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 76, 22);
        row.slider.setDoubleClickReturnValue (true, reset);
        row.slider.onDragStart = [this] { state.beginAction(); };
        row.slider.onValueChange = [this, &row]
        {
            if (! reading)
            {
                if (! row.slider.isMouseButtonDown())
                    state.beginAction();   // typed in, or double-clicked back

                apply (row);
            }
        };
        addAndMakeVisible (row.label);
        addAndMakeVisible (row.slider);
    }

    void readValues()
    {
        auto clip = findClip();

        if (! clip.isValid())
            return;

        const juce::ScopedValueSetter<bool> svs (reading, true);
        volume.slider.setValue (getClipGainDb (clip), juce::dontSendNotification);
        fadeIn.slider.setValue (getClipFadeIn (clip) * 1000.0, juce::dontSendNotification);
        fadeOut.slider.setValue (getClipFadeOut (clip) * 1000.0, juce::dontSendNotification);
        const double p = getClipPitch (clip);
        const double whole = std::round (p);
        pitch.slider.setValue (whole, juce::dontSendNotification);
        fine.slider.setValue (std::round ((p - whole) * 100.0), juce::dontSendNotification);
        speed.slider.setValue (getClipSpeed (clip) * 100.0, juce::dontSendNotification);
        keepPitch.setToggleState (getClipKeepsPitch (clip), juce::dontSendNotification);
    }

    void apply (Row& row)
    {
        auto clip = findClip();
        auto* edit = state.getEdit();

        if (! clip.isValid() || edit == nullptr)
            return;

        auto* um = &edit->getUndoManager();
        auto set = [&] (const juce::Identifier& id, double value, double defaultValue)
        {
            if (std::abs (value - defaultValue) < 1.0e-9) clip.removeProperty (id, um);
            else                                          clip.setProperty (id, value, um);
        };

        if (&row == &volume)
            set (ids::gain, volume.slider.getValue(), 0.0);
        else if (&row == &fadeIn || &row == &fadeOut)
            setClipFades (*edit, clip, fadeIn.slider.getValue() / 1000.0, fadeOut.slider.getValue() / 1000.0);
        else if (&row == &pitch || &row == &fine)
            set (ids::pitch, pitch.slider.getValue() + fine.slider.getValue() / 100.0, 0.0);
        else if (&row == &speed)
            setClipSpeed (*edit, clip, speed.slider.getValue() / 100.0);

        readValues();   // fades may have been shortened to fit
    }

    void fitToTempo()
    {
        auto clip = findClip();
        auto* edit = state.getEdit();
        const double itsBpm = bpmField.getText().replaceCharacter (',', '.').getDoubleValue();

        if (! clip.isValid() || edit == nullptr || itsBpm < 20.0 || itsBpm > 400.0)
            return;

        // The song's tempo where the clip starts.
        const auto& ts = edit->tempoSequence;
        const double songBpm = ts.getTempoAt (ts.toTime (te::BeatPosition::fromBeats ((double) clip[ids::start]))).getBpm();
        state.beginAction();
        setClipSpeed (*edit, clip, songBpm / itsBpm);
        readValues();
    }

    void resetAll()
    {
        auto clip = findClip();
        auto* edit = state.getEdit();

        if (! clip.isValid() || edit == nullptr)
            return;

        state.beginAction();
        auto* um = &edit->getUndoManager();
        setClipSpeed (*edit, clip, 1.0);

        for (auto* id : { &ids::gain, &ids::pitch, &ids::fi, &ids::fo, &ids::smode })
            clip.removeProperty (*id, um);

        readValues();
    }
};

void Playlist::showClipProperties (const juce::String& clipSid, juce::Rectangle<int> clipArea)
{
    auto panel = std::make_unique<ClipPropertiesPanel> (state, clipSid);

    if (panel->getWidth() == 0)
        return;   // the clip is gone

    // Inside the main window (not a see-through window of its own: those don't draw well everywhere).
    auto* top = getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::move (panel), top != nullptr ? top->getLocalArea (nullptr, clipArea) : clipArea, top);
}

void Playlist::bounceClip (const juce::String& clipSid)
{
    auto* edit = state.getEdit();
    auto* sync = state.session.getSync();

    if (edit == nullptr || sync == nullptr)
        return;

    if (bouncer == nullptr)
        bouncer = std::make_unique<Bouncer> (edit->engine, state.session.getAssetStore());

    const auto error = bouncer->start (sync->createSnapshot(), clipSid,
                                       [safe = juce::Component::SafePointer<Playlist> (this)] (const Bouncer::Result& r)
    {
        auto* e = safe != nullptr ? safe->state.getEdit() : nullptr;

        if (e == nullptr)
            return;

        if (! r.ok())
        {
            safe->state.session.logMessage (TRANS ("Bounce failed") + ": " + r.error);
            return;
        }

        safe->state.beginAction();
        const auto clips = Bouncer::apply (*e, r);

        if (clips.isEmpty())
        {
            safe->state.session.logMessage (TRANS ("Bounce: the clip was deleted meanwhile, nothing was replaced"));
            return;
        }

        safe->state.session.logMessage (TRANS ("Bounced into {N} audio clip(s) - they are being sent to the others (Ctrl+Z puts the clip back)")
                                          .replace ("{N}", juce::String (clips.size()))
                                        + (r.silentChannels > 0 ? " - " + TRANS ("{N} silent channel(s) left out").replace ("{N}", juce::String (r.silentChannels))
                                                                : juce::String()));
        safe->state.selectedChannel = safe->state.placeAudioChannel = clips.getFirst()[ids::ch].toString();

        if (safe->state.onSelectionChanged)
            safe->state.onSelectionChanged();

        safe->refresh();
    });

    state.session.logMessage (error.isNotEmpty() ? TRANS ("Bounce failed") + ": " + error : TRANS ("Bouncing to audio..."));
}

void Playlist::showImportDialog()
{
    chooser = std::make_unique<juce::FileChooser> (TRANS ("Import audio"), juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || fc.getResults().isEmpty())
            return;

        // First lane that is free at the song start marker.
        int lane = 0;
        const auto clips = getClips (*edit);

        for (; lane < numLanes - 1; ++lane)
        {
            bool busy = false;

            for (auto& c : clips)
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
                    routeTakeAndTell (safe->state, clip);
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
    const auto placing = audio != nullptr ? audio->getName() : state.getPattern()[ids::name].toString();
    title.setText (juce::String::fromUTF8 ("\xc2\xb7 ") + TRANS ("click to place") + " \"" + placing + "\"", juce::dontSendNotification);
    title.setTooltip (TRANS ("Click to place") + " \"" + placing + "\". " + TRANS ("Drag to move, drag the edges to resize, right-click to delete (right-click a clip's name: rename, colour, insert), Alt: no snap. Drop audio files here"));
    layoutContent();
    lanes->repaint();
    ruler->repaint();
    header->repaint();   // track names
}

void Playlist::paint (juce::Graphics& g)
{
    theme::fillPanel (g, getLocalBounds());
    auto top = getLocalBounds().removeFromTop (headerHeight).reduced (12, 0);
    theme::drawPanelTitle (g, top, TRANS ("Playlist"));
    g.setColour (theme::colours::line);
    g.fillRect (1, headerHeight - 1, getWidth() - 2, 1);
}

void Playlist::paintOverChildren (juce::Graphics& g)
{
    theme::framePanel (g, getLocalBounds());
}

void Playlist::resized()
{
    auto r = getLocalBounds().reduced (1);
    auto top = r.removeFromTop (headerHeight - 1).reduced (8, 5);
    fullScreenButton.setBounds (top.removeFromRight (30));
    top.removeFromRight (6);
    snapBox.setBounds (top.removeFromRight (112));
    top.removeFromRight (6);
    importButton.setBounds (top.removeFromRight (124));
    top.removeFromRight (6);
    automateButton.setBounds (top.removeFromRight (104));
    top.removeFromRight (8);
    sliceToolButton.setBounds (top.removeFromRight (30));
    top.removeFromRight (3);
    drawToolButton.setBounds (top.removeFromRight (30));
    top.removeFromRight (8);
    const int titleWidth = (int) juce::GlyphArrangement::getStringWidth (theme::ui (12.5f, theme::Weight::bold), TRANS ("Playlist")) + 10;
    title.setBounds (top.withTrimmedLeft (4 + titleWidth));

    auto rulerRow = r.removeFromTop (rulerHeight);
    rulerRow.removeFromLeft (laneHeaderWidth);
    ruler->setBounds (rulerRow.withTrimmedRight (viewport->getScrollBarThickness()));

    header->setBounds (r.removeFromLeft (laneHeaderWidth).withTrimmedBottom (viewport->getScrollBarThickness()));
    viewport->setBounds (r);
    layoutContent();
}

void Playlist::updateRows()
{
    rows.clear();
    rowOfLane.assign ((size_t) numLanes, 0);
    auto* edit = state.getEdit();
    const auto groups = edit != nullptr ? getLaneGroups (*edit) : juce::Array<LaneGroup>();
    int next = 0;   // the next group to meet

    for (int lane = 0; lane < numLanes;)
    {
        while (next < groups.size() && groups.getReference (next).last() < lane)
            ++next;

        if (next < groups.size() && groups.getReference (next).first == lane)
        {
            const auto& g = groups.getReference (next);
            const bool folded = state.foldedGroups.count (g.sid) > 0;
            const int headerRow = (int) rows.size();
            rows.push_back ({ -1, g, folded });

            for (int i = g.first; i <= g.last(); ++i)
            {
                if (folded)
                {
                    rowOfLane[(size_t) i] = headerRow;
                }
                else
                {
                    rowOfLane[(size_t) i] = (int) rows.size();
                    rows.push_back ({ i, g, false });
                }
            }

            lane = g.last() + 1;
            continue;
        }

        rowOfLane[(size_t) lane] = (int) rows.size();
        rows.push_back ({ lane, {}, false });
        ++lane;
    }
}

int Playlist::laneAtRowY (int y) const
{
    if (rows.empty())
        return juce::jlimit (0, numLanes - 1, y / laneHeight);

    const auto& r = rows[(size_t) rowAtY (y)];
    return r.lane >= 0 ? r.lane : r.group.first;
}

bool Playlist::isLaneHidden (int lane) const
{
    if (rows.empty() || ! juce::isPositiveAndBelow (lane, numLanes))
        return false;

    return rows[(size_t) rowOfLane[(size_t) lane]].lane != lane;
}

void Playlist::setGroupFolded (const juce::String& groupSid, bool folded)
{
    if (folded)
        state.foldedGroups.insert (groupSid);
    else
        state.foldedGroups.erase (groupSid);

    refresh();
}

void Playlist::layoutContent()
{
    updateRows();
    auto* edit = state.getEdit();
    const double songBeats = edit != nullptr ? getSongLengthBeats (*edit) : 4.0;
    const double bar = state.getBeatsPerBar();
    const double shownBeats = juce::jmax (64.0 * bar, songBeats + 16.0 * bar);
    lanes->setSize (juce::jmax (viewport->getMaximumVisibleWidth(), (int) (shownBeats * pixelsPerBeat)),
                    totalRowsHeight());
}

void Playlist::setZoom (float newPixelsPerBeat, float anchorX)
{
    newPixelsPerBeat = juce::jlimit (minPixelsPerBeat, 120.0f, newPixelsPerBeat);   // far out: a whole song on screen
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

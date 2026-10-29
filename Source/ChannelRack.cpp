#include "ChannelRack.h"
#include "DrumSynthPlugin.h"
#include "SamplerPlugin.h"
#include "FileImport.h"
#include "Look.h"
#include "Mixer.h"

static const juce::Colour stepOff = theme::colours::stepOff, stepOffAlt = theme::colours::stepOffAlt;


//==============================================================================
class ChannelRack::Row  : public juce::Component
{
public:
    Row (ChannelRack& r, const juce::String& id, int idx) : rack (r), channelID (id), index (idx)
    {
        for (auto* s : { &volume, &pan })
        {
            s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s->setRange (0.0, 1.0);
            s->setColour (juce::Slider::rotarySliderFillColourId, EditorState::channelColour (index));
            s->onDragStart = [this] { rack.state.beginAction(); };
            s->setLookAndFeel (&knobLook());
            addAndMakeVisible (s);
        }

        volume.setTooltip (TRANS ("Volume (right-click: reset to 0 dB, or automate it)"));
        pan.setTooltip (TRANS ("Pan (right-click: reset to centre, or automate it)"));
        pan.getProperties().set ("bipolar", true);
        pan.setColour (juce::Slider::rotarySliderFillColourId, theme::colours::muted);
        for (auto* k : { &volume, &pan })
            k->setColour (juce::Slider::rotarySliderOutlineColourId, theme::colours::stepOffAlt);
        rack.state.enableAutomation (volume, [this] { return (te::Plugin*) getVolume(); }, "volume");
        rack.state.enableAutomation (pan, [this] { return (te::Plugin*) getVolume(); }, "pan");
        volume.describeDefault = [] { return juce::String ("0 dB"); };
        pan.describeDefault = [] { return TRANS ("centre"); };
        volume.setDoubleClickReturnValue (true, 0.0);   // replaced in refresh() with the real default
        pan.setDoubleClickReturnValue (true, 0.5);

        volume.onValueChange = [this]
        {
            if (auto* v = getVolume())
                v->volParam->setNormalisedParameter ((float) volume.getValue(), juce::sendNotification);
        };

        pan.onValueChange = [this]
        {
            if (auto* v = getVolume())
                v->panParam->setNormalisedParameter ((float) pan.getValue(), juce::sendNotification);
        };

        mute.setClickingTogglesState (false);
        theme::setStyle (mute, "led");
        mute.setTooltip (TRANS ("Click: mute (for everyone). Right-click: solo (only what you hear)"));
        mute.onClick = [this]
        {
            auto* t = getTrack();

            if (t == nullptr)
                return;

            if (mute.wasRightClick())
            {
                // Solo is personal, like FL's right-click on the mute light.
                if (! rack.state.session.toggleSolo (channelID, false))
                    rack.state.session.logMessage (rack.state.session.getShareOwnerName()
                                                    + " controls what you hear right now (leave their share to solo)");

                rack.refresh();
                return;
            }

            rack.state.beginAction();
            t->setMute (! t->isMuted (false));
        };

        name.onClick = [this]
        {
            if (name.wasRightClick())
                rack.showChannelMenu (channelID);
            else
                select();
        };

        name.setTooltip (TRANS ("Click: select (piano roll below). Right-click: edit sound, effects, rename, delete. Audio channels: drag onto the playlist to place the file. Drop a sound from your folders here to give a sampler channel a new sound"));
        name.addMouseListener (this, false);   // to start dragging audio channels
        addAndMakeVisible (mute);
        addAndMakeVisible (name);

        route.setTooltip (TRANS ("Mixer insert this channel plays into (M = master). Click to change, or make a new insert"));
        route.onClick = [this]
        {
            if (auto* t = getTrack())
                Mixer::showRouteMenu (rack.state, *t, &route);
        };
        addAndMakeVisible (route);
    }

    te::AudioTrack* getTrack() const
    {
        auto* edit = rack.state.getEdit();
        return edit != nullptr ? song::findChannel (*edit, channelID) : nullptr;
    }

    te::VolumeAndPanPlugin* getVolume() const
    {
        auto* t = getTrack();
        return t != nullptr ? song::getVolumePlugin (*t) : nullptr;
    }

    void select()
    {
        rack.state.selectedChannel = channelID;

        // An audio channel becomes what the playlist places on click; any other
        // channel means you're working on patterns again.
        rack.state.placeAudioChannel = isAudio() ? channelID : juce::String();

        if (rack.state.onSelectionChanged)
            rack.state.onSelectionChanged();
    }

    void refresh()
    {
        auto* t = getTrack();

        if (t == nullptr)
            return;

        name.setButtonText (t->getName());
        const bool selected = rack.state.getSelectedChannel() == t;
        name.setColour (juce::TextButton::buttonColourId,
                        selected ? EditorState::channelColour (index) : theme::colours::well);
        name.setColour (juce::TextButton::textColourOffId, selected ? theme::colours::accentText : theme::colours::text);
        theme::setIcon (name, song::isSamplerChannel (*t) ? theme::Icon::wave : song::isAudioChannel (*t) ? theme::Icon::wave : theme::Icon::none);

        if (auto* edit = rack.state.getEdit())
        {
            auto* dest = song::resolveRoute (*edit, *t);
            route.setButtonText (dest != nullptr ? juce::String (song::getInsertNumber (*edit, *dest)) : juce::String ("M"));
            route.setColour (juce::TextButton::buttonColourId, dest != nullptr ? theme::colours::accentSoft : theme::colours::well);
            route.setColour (juce::TextButton::textColourOffId, dest != nullptr ? theme::colours::lilac : theme::colours::muted);
        }

        // Light: green = on, grey = muted, yellow = soloed, dark = silent here because of a solo.
        const bool muted = t->isMuted (false);
        const bool soloed = rack.state.session.isSoloed (channelID);
        const bool silenced = rack.state.session.isSilencedBySolo (channelID);
        mute.setButtonText ({});
        mute.setColour (juce::TextButton::buttonColourId, muted ? theme::colours::dim.withAlpha (0.6f)
                                                        : soloed ? theme::colours::amber
                                                        : silenced ? theme::colours::green.withAlpha (0.25f)
                                                                   : theme::colours::green);

        if (auto* v = getVolume())
        {
            if (! volume.isMouseButtonDown())
                volume.setValue (v->volParam->getCurrentNormalisedValue(), juce::dontSendNotification);

            if (! pan.isMouseButtonDown())
                pan.setValue (v->panParam->getCurrentNormalisedValue(), juce::dontSendNotification);

            auto range = v->volParam->getValueRange();
            volume.setDoubleClickReturnValue (true, (te::decibelsToVolumeFaderPosition (0.0f) - range.getStart()) / juce::jmax (0.0001f, range.getLength()));
        }

        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2, 3);
        mute.setBounds (r.removeFromLeft (16).withSizeKeepingCentre (16, 16));
        r.removeFromLeft (4);
        volume.setBounds (r.removeFromLeft (28));
        pan.setBounds (r.removeFromLeft (28));
        r.removeFromLeft (6);
        route.setBounds (r.removeFromLeft (30));
        r.removeFromLeft (6);
        name.setBounds (r.removeFromLeft (stepsX - 2 - 16 - 4 - 56 - 6 - 30 - 6 - 10));
    }

    juce::Rectangle<float> stepBounds (int step, int numSteps) const
    {
        const float x0 = (float) stepsX, w = (float) (getWidth() - stepsX - 4) / (float) numSteps;
        return { x0 + step * w, 3.0f, w, (float) getHeight() - 6.0f };
    }

    int stepAt (juce::Point<float> p, int numSteps) const
    {
        if (p.x < stepsX)
            return -1;

        const float w = (float) (getWidth() - stepsX - 4) / (float) numSteps;
        return juce::jlimit (0, numSteps - 1, (int) ((p.x - stepsX) / w));
    }

    bool isAudio() const
    {
        auto* t = getTrack();
        return t != nullptr && song::isAudioChannel (*t);
    }

    void paint (juce::Graphics& g) override
    {
        auto pattern = rack.state.getPattern();

        if (isAudio())
        {
            // Audio channels play their clips from the playlist, not from patterns.
            auto r = getLocalBounds().withTrimmedLeft (stepsX).reduced (2, 3).toFloat();
            g.setColour (EditorState::channelColour (index).withAlpha (0.14f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (theme::colours::muted);
            g.setFont (theme::ui (11.5f));
            g.drawText (TRANS ("Audio channel - its clips are in the playlist (SONG mode)"), r.reduced (10.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft, true);
            return;
        }

        if (! pattern.isValid())
            return;

        const int numSteps = song::getNumSteps (pattern);
        const int playhead = rack.state.getPlayheadStep();
        const auto colour = EditorState::channelColour (index);

        for (int s = 0; s < numSteps; ++s)
        {
            auto r = stepBounds (s, numSteps).reduced (1.5f, 0.0f);
            const bool on = song::hasStep (pattern, channelID, s);
            g.setColour (on ? colour : ((s / 4) % 2 == 0 ? stepOff : stepOffAlt));
            g.fillRoundedRectangle (r, 4.0f);

            if (s == playhead)
            {
                g.setColour (theme::colours::text.withAlpha (on ? 0.85f : 0.35f));
                g.drawRoundedRectangle (r.reduced (0.75f), 4.0f, 1.5f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != this)
            return;   // events from the name button (only listened to for dragging)

        auto pattern = rack.state.getPattern();
        const int step = stepAt (e.position, song::getNumSteps (pattern));
        auto* t = getTrack();
        lastStep = -1;

        if (step < 0 || t == nullptr || ! pattern.isValid())
            return;

        if (song::isAudioChannel (*t))
        {
            select();
            return;
        }

        rack.state.beginAction();
        paintValue = ! song::hasStep (pattern, channelID, step);
        lastStep = step;
        song::setStep (*rack.state.getEdit(), pattern, *t, step, paintValue);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Dragging an audio channel's name onto the playlist places its file.
        if (e.eventComponent == &name)
        {
            if (e.getDistanceFromDragStart() > 6 && isAudio())
                if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
                    if (! container->isDragAndDropActive())
                        container->startDragging ("audioChannel:" + channelID, &name);

            return;
        }

        if (e.eventComponent != this)
            return;

        // Drag across steps to paint them like in FL.
        auto pattern = rack.state.getPattern();
        const int step = stepAt (e.position, song::getNumSteps (pattern));
        auto* t = getTrack();

        if (step < 0 || step == lastStep || lastStep < 0 || t == nullptr || ! pattern.isValid() || song::isAudioChannel (*t))
            return;

        lastStep = step;
        song::setStep (*rack.state.getEdit(), pattern, *t, step, paintValue);
        repaint();
    }

    ~Row() override
    {
        volume.setLookAndFeel (nullptr);
        pan.setLookAndFeel (nullptr);
    }

    ChannelRack& rack;
    const juce::String channelID;
    const int index;
    MenuButton mute;
    juce::TextButton route;
    MenuButton name;
    ResetSlider volume, pan;
    bool paintValue = true;
    int lastStep = -1;
};

//==============================================================================
ChannelRack::ChannelRack (te::Engine& e, EditorState& s) : engine (e), state (s)
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    addAndMakeVisible (addButton);
    addButton.onClick = [this] { showAddChannelMenu(); };

    // Like FL's channel filter: see only the instruments (your patterns) or only the audio (your takes).
    filterBox.addItem (TRANS ("Show: all channels"), 1);
    filterBox.addItem (TRANS ("Show: instruments"), 2);
    filterBox.addItem (TRANS ("Show: audio (takes)"), 3);
    filterBox.setSelectedId (1, juce::dontSendNotification);
    filterBox.setTooltip (TRANS ("Which channels the rack shows (only on your screen)"));
    theme::setIcon (addButton, theme::Icon::plus);
    filterBox.onChange = [this]
    {
        const int id = filterBox.getSelectedId();
        state.rackFilter = id == 2 ? EditorState::RackFilter::instruments
                         : id == 3 ? EditorState::RackFilter::audio : EditorState::RackFilter::all;
        refresh();
    };
    addAndMakeVisible (filterBox);

    for (auto* b : { &detachButton, &fullScreenButton })
        b->getProperties().set ("iconOnly", true);

    detachButton.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
    fullScreenButton.onClick = [this] { if (onFullScreenToggle) onFullScreenToggle(); };
    addAndMakeVisible (detachButton);
    addChildComponent (fullScreenButton);
    setDetached (false);
    startTimerHz (30);
}

void ChannelRack::setDetached (bool detached)
{
    theme::setIcon (detachButton, detached ? theme::Icon::attach : theme::Icon::detach);
    detachButton.setTooltip (detached ? TRANS ("Put the channel rack back in the main window (closing its window does the same)")
                                      : TRANS ("Give the channel rack a window of its own - e.g. on your second monitor"));
    fullScreenButton.setVisible (detached);
    setFullScreenState (false);
    resized();
}

void ChannelRack::setFullScreenState (bool isFullScreen)
{
    theme::setIcon (fullScreenButton, isFullScreen ? theme::Icon::exitFullScreen : theme::Icon::fullScreen);
    fullScreenButton.setTooltip (isFullScreen ? TRANS ("Back to the normal size") : TRANS ("Fill the whole screen with the channel rack"));
    fullScreenButton.repaint();
}

ChannelRack::~ChannelRack()
{
    stopTimer();
    importPool.removeAllJobs (true, 10000);
}

void ChannelRack::refresh()
{
    auto* edit = state.getEdit();
    juce::StringArray ids;
    juce::Array<int> colourIndexes;   // position in the full list, so colours don't change with the filter

    if (edit != nullptr)
    {
        auto channels = song::getChannels (*edit);

        for (int i = 0; i < channels.size(); ++i)
        {
            const bool audio = song::isAudioChannel (*channels[i]);

            if ((state.rackFilter == EditorState::RackFilter::instruments && audio)
                 || (state.rackFilter == EditorState::RackFilter::audio && ! audio))
                continue;

            ids.add (song::getChannelID (*channels[i]));
            colourIndexes.add (i);
        }
    }

    if (ids != rowIDs || colourIndexes != rowColourIndexes)
    {
        rows.clear();
        rowIDs = ids;
        rowColourIndexes = colourIndexes;

        for (int i = 0; i < ids.size(); ++i)
            content.addAndMakeVisible (rows.add (new Row (*this, ids[i], colourIndexes[i])));

        layoutRows();
    }

    // A channel just added from a file: bring it into view.
    if (scrollToChannel.isNotEmpty())
    {
        const int index = rowIDs.indexOf (scrollToChannel);

        if (index >= 0)
        {
            const int top = index * rowHeight;
            const int viewTop = viewport.getViewPositionY();

            if (top < viewTop || top + rowHeight > viewTop + viewport.getViewHeight())
                viewport.setViewPosition (0, juce::jmax (0, top + rowHeight - viewport.getViewHeight()));

            scrollToChannel = {};
        }
    }

    for (auto* r : rows)
        r->refresh();

    repaint (getLocalBounds().removeFromBottom (bottomHeight));
}

void ChannelRack::paint (juce::Graphics& g)
{
    theme::fillPanel (g, getLocalBounds());

    // The bar at the bottom: a line above it, and what you're editing on the right.
    auto bottom = getLocalBounds().removeFromBottom (bottomHeight);
    g.setColour (theme::colours::line);
    g.fillRect (bottom.removeFromTop (1).reduced (1, 0));

    auto pattern = state.getPattern();

    if (pattern.isValid())
    {
        const int bars = song::getBars (pattern);
        g.setColour (theme::colours::muted);
        g.setFont (theme::ui (11.0f));
        g.drawText (pattern[song::ids::name].toString() + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (bars) + " "
                      + (bars == 1 ? TRANS ("bar") : TRANS ("bars")) + juce::String::fromUTF8 (" \xc2\xb7 ")
                      + juce::String (song::getNumSteps (pattern)) + " " + TRANS ("steps"),
                    bottom.reduced (12, 0).withTrimmedLeft (filterBox.getRight()).withRight (detachButton.getX() - 8),
                    juce::Justification::centredRight, true);
    }
}

void ChannelRack::resized()
{
    auto r = getLocalBounds().reduced (1);
    auto bottom = r.removeFromBottom (bottomHeight - 1).reduced (10, 5);
    addButton.setBounds (bottom.removeFromLeft (112));
    bottom.removeFromLeft (8);
    filterBox.setBounds (bottom.removeFromLeft (170));

    if (fullScreenButton.isVisible())
    {
        fullScreenButton.setBounds (bottom.removeFromRight (30));
        bottom.removeFromRight (4);
    }

    detachButton.setBounds (bottom.removeFromRight (30));
    viewport.setBounds (r.reduced (8, 6));
    layoutRows();
}

void ChannelRack::layoutRows()
{
    const int w = viewport.getWidth() - (rows.size() * rowHeight > viewport.getHeight() ? viewport.getScrollBarThickness() : 0);
    content.setSize (juce::jmax (100, w), rows.size() * rowHeight);

    for (int i = 0; i < rows.size(); ++i)
        rows[i]->setBounds (0, i * rowHeight, content.getWidth(), rowHeight);
}

void ChannelRack::timerCallback()
{
    // Playhead and knobs moved by other people.
    const int playhead = state.getPlayheadStep();
    const bool playheadMoved = playhead != lastPlayhead;
    lastPlayhead = playhead;

    static int tick = 0;

    if (playheadMoved || ++tick % 6 == 0)
        for (auto* r : rows)
            r->refresh();
}

void ChannelRack::paintOverChildren (juce::Graphics& g)
{
    theme::framePanel (g, getLocalBounds());

    if (! drop.active)
        return;

    // Where the dragged file would go: onto a channel, or a new channel at the end.
    const bool midi = fileimport::isMidiFile (drop.file);
    juce::Rectangle<int> area;
    juce::String text;
    auto* edit = state.getEdit();
    auto* target = edit != nullptr && drop.channelID.isNotEmpty() ? song::findChannel (*edit, drop.channelID) : nullptr;

    if (target != nullptr)
    {
        for (auto* row : rows)
            if (row->channelID == drop.channelID)
                area = getLocalArea (&content, row->getBounds()).getIntersection (viewport.getBounds());

        text = midi ? TRANS ("Notes into") + " " + target->getName() + " (" + TRANS ("this pattern") + ")" : TRANS ("New sound for") + " " + target->getName();
    }
    else
    {
        // A new channel: the whole rack lights up, the label sits at the bottom.
        area = viewport.getBounds().reduced (1);
        text = midi ? TRANS ("New Synth channel:") + " " + song::channelNameForFile (drop.file.getFileName())
                    : TRANS ("New channel:") + " " + song::channelNameForFile (drop.file.getFileName());
    }

    if (area.isEmpty())
        return;

    const auto colour = theme::colours::accent;
    g.setColour (theme::colours::accentSoft.withAlpha (target != nullptr ? 0.85f : 0.5f));
    g.fillRoundedRectangle (area.toFloat(), 7.0f);
    g.setColour (colour);
    juce::Path outline;
    outline.addRoundedRectangle (area.toFloat().reduced (1.0f), 7.0f);
    juce::Path dashed;
    const float dashes[] = { 6.0f, 4.0f };
    juce::PathStrokeType (2.0f).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);

    auto label = target != nullptr ? area.withTrimmedLeft (stepsX).reduced (8, 0)
                                   : area.removeFromBottom (rowHeight).withTrimmedLeft (stepsX).reduced (8, 0);

    if (label.getWidth() < 120)
        label = area.reduced (8, 0);

    const auto font = theme::ui (12.5f, theme::Weight::bold);
    const auto textWidth = juce::jmin (label.getWidth(), juce::GlyphArrangement::getStringWidthInt (font, text) + 18);
    g.setColour (theme::colours::accent);
    g.fillRoundedRectangle (label.withWidth (textWidth).withSizeKeepingCentre (textWidth, 22).toFloat(), 6.0f);
    g.setColour (theme::colours::accentText);
    g.setFont (font);
    g.drawText (text, label.withWidth (textWidth), juce::Justification::centred, true);
}

ChannelRack::DropTarget ChannelRack::dropTargetAt (const juce::File& file, juce::Point<int> pos) const
{
    DropTarget t;
    t.file = file;
    t.active = fileimport::isAudioFile (file) || fileimport::isMidiFile (file);
    auto* edit = state.getEdit();

    if (! t.active || edit == nullptr || ! viewport.getBounds().contains (pos))
        return t;

    const auto inContent = content.getLocalPoint (this, pos);
    const int index = inContent.y / rowHeight;

    if (! juce::isPositiveAndBelow (index, rows.size()))
        return t;   // below the last channel: a new one

    auto* track = song::findChannel (*edit, rows[index]->channelID);

    if (track == nullptr)
        return t;

    // A sound replaces a sampler channel's sound; MIDI goes onto any channel that plays notes.
    // Anything else makes a new channel.
    if (fileimport::isMidiFile (file) ? ! song::isAudioChannel (*track) : song::isSamplerChannel (*track))
        t.channelID = rows[index]->channelID;

    return t;
}

void ChannelRack::setDrop (DropTarget t)
{
    if (t.active != drop.active || t.channelID != drop.channelID || t.file != drop.file)
    {
        drop = t;
        repaint();
    }
}

void ChannelRack::dropFile (const juce::File& file, juce::Point<int> pos)
{
    const auto target = dropTargetAt (file, pos);

    if (! target.active)
        return;

    if (fileimport::isMidiFile (file))
        importMidiFile (file, target.channelID);
    else
        addSoundFile (file, target.channelID);
}

bool ChannelRack::isInterestedInDragSource (const SourceDetails& d)
{
    const auto f = fileimport::draggedFile (d.description);
    return fileimport::isAudioFile (f) || fileimport::isMidiFile (f);
}

void ChannelRack::itemDragMove (const SourceDetails& d)     { setDrop (dropTargetAt (fileimport::draggedFile (d.description), d.localPosition.toInt())); }
void ChannelRack::itemDragExit (const SourceDetails&)       { setDrop ({}); }

void ChannelRack::itemDropped (const SourceDetails& d)
{
    setDrop ({});
    dropFile (fileimport::draggedFile (d.description), d.localPosition.toInt());
}

bool ChannelRack::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (fileimport::isAudioFile (juce::File (f)) || fileimport::isMidiFile (juce::File (f)))
            return true;

    return false;
}

void ChannelRack::fileDragMove (const juce::StringArray& files, int x, int y)
{
    for (auto& f : files)
        if (fileimport::isAudioFile (juce::File (f)) || fileimport::isMidiFile (juce::File (f)))
        {
            setDrop (dropTargetAt (juce::File (f), { x, y }));
            return;
        }
}

void ChannelRack::fileDragExit (const juce::StringArray&)   { setDrop ({}); }

void ChannelRack::filesDropped (const juce::StringArray& files, int x, int y)
{
    setDrop ({});
    bool first = true;

    for (auto& f : files)
    {
        const juce::File file (f);

        if (! fileimport::isAudioFile (file) && ! fileimport::isMidiFile (file))
            continue;

        // The first file goes where it was dropped; any others become new channels.
        if (first)
            dropFile (file, { x, y });
        else if (fileimport::isMidiFile (file))
            importMidiFile (file);
        else
            addSoundFile (file);

        first = false;
    }
}

void ChannelRack::addSoundFile (const juce::File& file, const juce::String& ontoChannelID)
{
    auto* store = &state.session.getAssetStore();
    state.session.logMessage ("Adding " + file.getFileName() + " to the channel rack...");
    juce::Component::SafePointer<ChannelRack> safe (this);   // made here, on the message thread

    importPool.addJob ([safe, file, ontoChannelID, store]
    {
        auto result = store->importAudioFile (file);   // converts to FLAC (background thread)

        juce::MessageManager::callAsync ([safe, file, ontoChannelID, result]
        {
            if (safe != nullptr)
                safe->soundImported (file, result, ontoChannelID);
        });
    });
}

void ChannelRack::soundImported (const juce::File& file, const AssetStore::ImportResult& result, const juce::String& ontoChannelID)
{
    auto* edit = state.getEdit();

    if (! result.ok() || edit == nullptr)
    {
        state.session.logMessage (result.error.isNotEmpty() ? result.error : "Couldn't add " + file.getFileName());
        return;
    }

    state.beginAction();
    auto* target = ontoChannelID.isNotEmpty() ? song::findChannel (*edit, ontoChannelID) : nullptr;
    const auto oldName = target != nullptr ? target->getName() : juce::String();

    if (target != nullptr && song::setSamplerSample (*edit, *target, result, file.getFileName()))
    {
        state.session.logMessage ("New sound for " + oldName + ": " + file.getFileName()
                                   + " - it is being sent to the others");
    }
    else
    {
        target = song::addSamplerChannel (*edit, result, file.getFileName());

        if (target != nullptr)
            state.session.logMessage ("Added " + file.getFileName() + " to the channel rack ("
                                       + juce::String (result.lengthSeconds, 2) + " s) - it is being sent to the others");
    }

    if (target != nullptr)
    {
        state.selectedChannel = scrollToChannel = song::getChannelID (*target);
        state.placeAudioChannel = {};

        if (state.onSelectionChanged)
            state.onSelectionChanged();
    }
}

void ChannelRack::importMidiFile (const juce::File& file, const juce::String& ontoChannelID)
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    state.beginAction();
    auto channelID = ontoChannelID;
    state.session.logMessage (fileimport::applyMidiFile (*edit, state.getPattern(), file, channelID));

    if (song::findChannel (*edit, channelID) != nullptr)
    {
        state.selectedChannel = scrollToChannel = channelID;
        state.placeAudioChannel = {};

        if (state.onSelectionChanged)
            state.onSelectionChanged();
    }
}

void ChannelRack::showAddChannelMenu()
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    juce::PopupMenu m, vst;
    m.addSectionHeader (TRANS ("Built-in"));
    m.addItem (1, TRANS ("Kick (Drum Synth)"));
    m.addItem (2, TRANS ("Clap (Drum Synth)"));
    m.addItem (3, TRANS ("Hat (Drum Synth)"));
    m.addItem (4, TRANS ("Snare (Drum Synth)"));
    m.addItem (5, TRANS ("Synth (4OSC)"));
    m.addItem (6, TRANS ("Sampler: choose a sound file..."));

    juce::Array<juce::PluginDescription> instruments;

    for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
        if (d.isInstrument)
            instruments.add (d);

    if (instruments.isEmpty())
        vst.addItem (-1, TRANS ("(none found - use Scan plugins...)"), false);
    else
        juce::KnownPluginList::addToMenu (vst, instruments, juce::KnownPluginList::sortByManufacturer);

    m.addSubMenu (TRANS ("Your instruments (VST3)"), vst);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton),
                     [this, instruments] (int result)
    {
        auto* e = state.getEdit();

        if (e == nullptr || result == 0)
            return;

        if (result == 6)
        {
            chooser = std::make_unique<juce::FileChooser> (TRANS ("Sound for the new channel"), juce::File(),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
            {
                if (fc.getResult().existsAsFile())
                    addSoundFile (fc.getResult());
            });
            return;
        }

        state.beginAction();
        te::AudioTrack* track = nullptr;

        if (result >= 1 && result <= 5)
        {
            const song::InstrumentKind kinds[] = { song::InstrumentKind::kick, song::InstrumentKind::clap,
                                                   song::InstrumentKind::hat, song::InstrumentKind::snare,
                                                   song::InstrumentKind::synth };
            track = song::addChannel (*e, kinds[result - 1]);
        }
        else
        {
            auto index = juce::KnownPluginList::getIndexChosenByMenu (instruments, result);

            if (juce::isPositiveAndBelow (index, instruments.size()))
                track = song::addChannel (*e, instruments[index]);
        }

        if (track != nullptr)
        {
            state.selectedChannel = song::getChannelID (*track);

            if (state.onSelectionChanged)
                state.onSelectionChanged();
        }
    });
}

void ChannelRack::showChannelMenu (const juce::String& channelID)
{
    auto* edit = state.getEdit();
    auto* track = edit != nullptr ? song::findChannel (*edit, channelID) : nullptr;

    if (track == nullptr)
        return;

    juce::PopupMenu m, addFx, fx;
    auto* instrument = song::getInstrument (*track);
    const bool missing = [instrument]
    {
        auto* ext = dynamic_cast<te::ExternalPlugin*> (instrument);
        return ext != nullptr && ext->getAudioPluginInstance() == nullptr;
    }();

    // Like FL: jump straight to this channel's notes, with the piano roll full screen.
    m.addItem (6, TRANS ("Piano roll (full screen)"), ! song::isAudioChannel (*track));
    m.addSeparator();

    // Our own instruments: "Edit sound..." (FL's channel settings). Plugins: their own window.
    const bool ours = dynamic_cast<SamplerPlugin*> (instrument) != nullptr || dynamic_cast<DrumSynthPlugin*> (instrument) != nullptr;
    m.addItem (1, ours ? TRANS ("Edit sound...")
                       : instrument != nullptr ? TRANS ("Open instrument") + " (" + instrument->getName() + (missing ? ", " + TRANS ("not installed here") + ")" : ")")
                                               : (song::isAudioChannel (*track) ? TRANS ("Audio channel (no instrument)") : TRANS ("No instrument")),
               instrument != nullptr && ! missing);

    if (auto* sampler = dynamic_cast<SamplerPlugin*> (instrument))
    {
        const auto hash = sampler->getAsset();
        const bool here = state.session.getAssetStore().has (hash);
        m.addItem (-2, TRANS ("Sound:") + " " + song::getAssetName (*edit, hash) + (here ? juce::String() : "  (" + TRANS ("still arriving...") + ")"), false);
        m.addItem (4, TRANS ("Cut itself (a new hit stops the last one, for 808s)"), true, sampler->cutValue.get());
        m.addItem (5, TRANS ("Replace the sound...  (or drop one on this channel)"));
    }

    auto effects = song::getEffects (*track);

    for (int i = 0; i < effects.size(); ++i)
    {
        juce::PopupMenu one;
        one.addItem (100 + i, TRANS ("Open"));
        one.addItem (200 + i, TRANS ("Remove"));
        fx.addSubMenu (effects[i]->getName(), one);
    }

    if (effects.isEmpty())
        fx.addItem (-1, TRANS ("(no effects yet)"), false);

    juce::Array<juce::PluginDescription> effectTypes;

    for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
        if (! d.isInstrument)
            effectTypes.add (d);

    if (effectTypes.isEmpty())
        addFx.addItem (-1, TRANS ("(none found - use Scan plugins...)"), false);
    else
        juce::KnownPluginList::addToMenu (addFx, effectTypes, juce::KnownPluginList::sortByManufacturer);

    m.addSubMenu (TRANS ("Effects"), fx);
    m.addSubMenu (TRANS ("Add effect (Waves, FabFilter...)"), addFx);
    m.addSeparator();
    m.addItem (2, TRANS ("Rename..."));
    m.addItem (3, TRANS ("Delete channel"), song::getChannels (*edit).size() > 1);

    m.showMenuAsync ({}, [this, channelID, effectTypes] (int result)
    {
        auto* e = state.getEdit();
        auto* t = e != nullptr ? song::findChannel (*e, channelID) : nullptr;

        if (t == nullptr || result == 0)
            return;

        if (result == 6)
        {
            if (onOpenPianoRoll)
                onOpenPianoRoll (channelID);
        }
        else if (result == 1)
        {
            if (auto* inst = song::getInstrument (*t))
                inst->showWindowExplicitly();
        }
        else if (result == 2)
        {
            auto* w = new juce::AlertWindow (TRANS ("Rename channel"), {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", t->getName());
            w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([this, channelID, w] (int r)
            {
                auto* e2 = state.getEdit();

                if (r == 1 && e2 != nullptr)
                    if (auto* t2 = song::findChannel (*e2, channelID))
                    {
                        state.beginAction();
                        t2->setName (w->getTextEditorContents ("name").trim().substring (0, 40));
                    }
            }), true);
        }
        else if (result == 3)
        {
            state.beginAction();
            song::removeChannel (*e, *t);
        }
        else if (result == 4)
        {
            if (auto* sampler = dynamic_cast<SamplerPlugin*> (song::getInstrument (*t)))
            {
                state.beginAction();
                sampler->cutValue = ! sampler->cutValue.get();
            }
        }
        else if (result == 5)
        {
            chooser = std::make_unique<juce::FileChooser> (TRANS ("New sound for") + " " + t->getName(), juce::File(),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, channelID] (const juce::FileChooser& fc)
            {
                if (fc.getResult().existsAsFile())
                    addSoundFile (fc.getResult(), channelID);
            });
        }
        else if (result >= 100 && result < 200)
        {
            if (auto* p = song::getEffects (*t)[result - 100])
                p->showWindowExplicitly();
        }
        else if (result >= 200 && result < 300)
        {
            if (auto* p = song::getEffects (*t)[result - 200])
            {
                state.beginAction();
                p->deleteFromParent();
            }
        }
        else
        {
            auto index = juce::KnownPluginList::getIndexChosenByMenu (effectTypes, result);

            if (juce::isPositiveAndBelow (index, effectTypes.size()))
            {
                state.beginAction();
                song::addEffect (*t, effectTypes[index]);
            }
        }
    });
}

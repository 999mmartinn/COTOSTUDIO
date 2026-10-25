#include "ChannelRack.h"
#include "DrumSynthPlugin.h"
#include "Look.h"
#include "Mixer.h"

static const juce::Colour rackBackground (0xff2a2f36), stepOff (0xff3a414a), stepOffAlt (0xff464e59);


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

        volume.setTooltip ("Volume (right-click: reset to 0 dB)");
        pan.setTooltip ("Pan (right-click: reset to centre)");
        volume.describeDefault = [] { return juce::String ("0 dB"); };
        pan.describeDefault = [] { return juce::String ("centre"); };
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
        mute.setTooltip ("Mute");
        mute.onClick = [this]
        {
            if (auto* t = getTrack())
            {
                rack.state.beginAction();
                t->setMute (! t->isMuted (false));
            }
        };

        name.onClick = [this]
        {
            if (juce::ModifierKeys::currentModifiers.isPopupMenu())
                rack.showChannelMenu (channelID);
            else
                select();
        };

        name.setTooltip ("Click: select (piano roll below). Right-click: instrument, effects, rename, delete."
                         " Audio channels: drag onto the playlist to place the file");
        name.addMouseListener (this, false);   // to start dragging audio channels
        addAndMakeVisible (mute);
        addAndMakeVisible (name);

        route.setTooltip ("Mixer insert this channel plays into (M = master). Click to change, or make a new insert");
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
                        selected ? EditorState::channelColour (index).darker (0.2f) : juce::Colour (0xff3a414a));
        name.setColour (juce::TextButton::textColourOffId, selected ? juce::Colours::black : juce::Colours::white);

        if (auto* edit = rack.state.getEdit())
        {
            auto* dest = song::resolveRoute (*edit, *t);
            route.setButtonText (dest != nullptr ? juce::String (song::getInsertNumber (*edit, *dest)) : juce::String ("M"));
            route.setColour (juce::TextButton::buttonColourId, dest != nullptr ? juce::Colour (0xff5a4a2a) : juce::Colour (0xff3a414a));
        }

        const bool muted = t->isMuted (false);
        mute.setButtonText (muted ? "" : "");
        mute.setColour (juce::TextButton::buttonColourId, muted ? juce::Colour (0xff555555) : juce::Colour (0xff6fd06f));

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
        mute.setBounds (r.removeFromLeft (14).withSizeKeepingCentre (14, 14));
        r.removeFromLeft (6);
        volume.setBounds (r.removeFromLeft (28));
        pan.setBounds (r.removeFromLeft (28));
        r.removeFromLeft (6);
        route.setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        name.setBounds (r.removeFromLeft (stepsX - 6 - 14 - 6 - 56 - 6 - 30 - 4 - 4));
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
            g.setColour (EditorState::channelColour (index).withAlpha (0.18f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.setFont (12.0f);
            g.drawText ("Audio channel - its clips are in the playlist (SONG mode)", r.reduced (8.0f, 0.0f).toNearestInt(),
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
            g.fillRoundedRectangle (r, 3.0f);

            if (s == playhead)
            {
                g.setColour (juce::Colours::white.withAlpha (on ? 0.6f : 0.25f));
                g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.5f);
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
    juce::TextButton mute, name, route;
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
    startTimerHz (30);
}

ChannelRack::~ChannelRack()
{
    stopTimer();
}

void ChannelRack::refresh()
{
    auto* edit = state.getEdit();
    juce::StringArray ids;

    if (edit != nullptr)
        for (auto* t : song::getChannels (*edit))
            ids.add (song::getChannelID (*t));

    if (ids != rowIDs)
    {
        rows.clear();
        rowIDs = ids;

        for (int i = 0; i < ids.size(); ++i)
            content.addAndMakeVisible (rows.add (new Row (*this, ids[i], i)));

        layoutRows();
    }

    for (auto* r : rows)
        r->refresh();
}

void ChannelRack::paint (juce::Graphics& g)
{
    g.setColour (rackBackground);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void ChannelRack::resized()
{
    auto r = getLocalBounds().reduced (4);
    addButton.setBounds (r.removeFromBottom (26).removeFromLeft (110));
    r.removeFromBottom (4);
    viewport.setBounds (r);
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

void ChannelRack::showAddChannelMenu()
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    juce::PopupMenu m, vst;
    m.addSectionHeader ("Built-in");
    m.addItem (1, "Kick (Drum Synth)");
    m.addItem (2, "Clap (Drum Synth)");
    m.addItem (3, "Hat (Drum Synth)");
    m.addItem (4, "Snare (Drum Synth)");
    m.addItem (5, "Synth (4OSC)");

    juce::Array<juce::PluginDescription> instruments;

    for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
        if (d.isInstrument)
            instruments.add (d);

    if (instruments.isEmpty())
        vst.addItem (-1, "(none found - use Scan plugins...)", false);
    else
        juce::KnownPluginList::addToMenu (vst, instruments, juce::KnownPluginList::sortByManufacturer);

    m.addSubMenu ("Your instruments (VST3)", vst);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton),
                     [this, instruments] (int result)
    {
        auto* e = state.getEdit();

        if (e == nullptr || result == 0)
            return;

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

    m.addItem (1, instrument != nullptr ? "Open instrument (" + instrument->getName() + (missing ? ", not installed here)" : ")")
                                        : juce::String (song::isAudioChannel (*track) ? "Audio channel (no instrument)" : "No instrument"),
               instrument != nullptr && ! missing);

    auto effects = song::getEffects (*track);

    for (int i = 0; i < effects.size(); ++i)
    {
        juce::PopupMenu one;
        one.addItem (100 + i, "Open");
        one.addItem (200 + i, "Remove");
        fx.addSubMenu (effects[i]->getName(), one);
    }

    if (effects.isEmpty())
        fx.addItem (-1, "(no effects yet)", false);

    juce::Array<juce::PluginDescription> effectTypes;

    for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
        if (! d.isInstrument)
            effectTypes.add (d);

    if (effectTypes.isEmpty())
        addFx.addItem (-1, "(none found - use Scan plugins...)", false);
    else
        juce::KnownPluginList::addToMenu (addFx, effectTypes, juce::KnownPluginList::sortByManufacturer);

    m.addSubMenu ("Effects", fx);
    m.addSubMenu ("Add effect (Waves, FabFilter...)", addFx);
    m.addSeparator();
    m.addItem (2, "Rename...");
    m.addItem (3, "Delete channel", song::getChannels (*edit).size() > 1);

    m.showMenuAsync ({}, [this, channelID, effectTypes] (int result)
    {
        auto* e = state.getEdit();
        auto* t = e != nullptr ? song::findChannel (*e, channelID) : nullptr;

        if (t == nullptr || result == 0)
            return;

        if (result == 1)
        {
            if (auto* inst = song::getInstrument (*t))
                inst->showWindowExplicitly();
        }
        else if (result == 2)
        {
            auto* w = new juce::AlertWindow ("Rename channel", {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", t->getName());
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

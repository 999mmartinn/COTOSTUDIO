#include "Mixer.h"
#include "Look.h"

using namespace song;

static const juce::Colour panelColour (0xff2a2f36), stripColour (0xff31373f), selectedStrip (0xff3d4550), accent (0xfff28c28);

//==============================================================================
/** A vertical peak meter reading an engine LevelMeasurer (local only, nothing is sent). */
class MeterBar  : public juce::Component
{
public:
    ~MeterBar() override    { detach(); }

    void update (te::LevelMeasurer* measurer)
    {
        if (measurer != attached.get())
        {
            detach();

            if (measurer != nullptr)
            {
                measurer->addClient (client);
                attached = measurer;
            }
        }

        float db = -100.0f;

        if (attached != nullptr)
            for (int ch = 0; ch < 2; ++ch)
                db = juce::jmax (db, client.getAndClearAudioLevel (ch).dB);

        // Fast attack, slow fall.
        const float target = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        level = target > level ? target : juce::jmax (target, level - 0.025f);
        clipped = db > 0.0f || (clipped && ++clipHold < 60);

        if (db > 0.0f)
            clipHold = 0;

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff15181c));
        g.fillRoundedRectangle (r, 2.0f);

        auto filled = r.reduced (1.0f);
        filled = filled.removeFromBottom (filled.getHeight() * level);
        juce::ColourGradient grad (juce::Colour (0xffe04040), r.getX(), r.getY(),
                                   juce::Colour (0xff4fc04f), r.getX(), r.getBottom(), false);
        grad.addColour (0.25, juce::Colour (0xffe0c040));
        g.setGradientFill (grad);
        g.fillRect (filled);

        if (clipped)
        {
            g.setColour (juce::Colour (0xffff3030));
            g.fillRect (r.removeFromTop (4.0f));
        }
    }

private:
    te::LevelMeasurer::Client client;
    juce::WeakReference<te::LevelMeasurer> attached;
    float level = 0.0f;
    bool clipped = false;
    int clipHold = 0;

    void detach()
    {
        if (auto* m = attached.get())
            m->removeClient (client);

        attached = nullptr;
    }
};

//==============================================================================
class Mixer::Strip  : public juce::Component
{
public:
    Strip (Mixer& m, const juce::String& id) : mixer (m), insertID (id)
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (0.0, 1.0);
        fader.setColour (juce::Slider::trackColourId, accent);
        fader.setColour (juce::Slider::thumbColourId, juce::Colours::white);
        fader.onDragStart = [this] { mixer.state.beginAction(); };
        fader.onValueChange = [this]
        {
            if (auto* v = getVolume())
                v->volParam->setNormalisedParameter ((float) fader.getValue(), juce::sendNotification);
        };

        pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (0.0, 1.0);
        pan.setDoubleClickReturnValue (true, 0.5);
        pan.setColour (juce::Slider::rotarySliderFillColourId, accent);
        pan.setLookAndFeel (&knobLook());
        pan.setTooltip ("Pan (right-click: reset to centre)");
        pan.describeDefault = [] { return juce::String ("centre"); };
        fader.setTooltip (isMaster() ? "Volume (right-click: reset to -3 dB)" : "Volume (right-click: reset to 0 dB)");
        fader.describeDefault = [this] { return juce::String (isMaster() ? "-3 dB" : "0 dB"); };
        pan.onDragStart = [this] { mixer.state.beginAction(); };
        pan.onValueChange = [this]
        {
            if (auto* v = getVolume())
                v->panParam->setNormalisedParameter ((float) pan.getValue(), juce::sendNotification);
        };

        name.onClick = [this]
        {
            select();

            if (juce::ModifierKeys::currentModifiers.isPopupMenu())
                showMenu();
        };

        mute.setTooltip ("Mute");
        mute.onClick = [this]
        {
            if (auto* t = getTrack())
            {
                mixer.state.beginAction();
                t->setMute (! t->isMuted (false));
            }
        };

        input.setTooltip ("Audio input recorded into this insert (your interface / mic). "
                          "You hear it through this insert's effects");
        input.onClick = [this] { showInputMenu(); };
        arm.setTooltip ("Arm for recording: the Record button records this insert's input");
        arm.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x8f"));   // a dot
        arm.onClick = [this]
        {
            auto& rec = mixer.state.session.getRecorder();

            if (rec.getInputFor (insertID).isEmpty())
                showInputMenu();
            else
                rec.setArmed (insertID, ! rec.isArmed (insertID));

            refresh();
        };

        route.setTooltip ("Where this insert's sound goes");
        route.onClick = [this]
        {
            if (auto* t = getTrack())
                Mixer::showRouteMenu (mixer.state, *t, &route);
        };

        for (auto* c : std::initializer_list<juce::Component*> { &name, &meter, &fader, &pan, &dbLabel })
            addAndMakeVisible (c);

        if (! isMaster())
        {
            addAndMakeVisible (mute);
            addAndMakeVisible (route);
            addAndMakeVisible (input);
            addAndMakeVisible (arm);
        }

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::FontOptions (11.0f));
        dbLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    }

    ~Strip() override    { pan.setLookAndFeel (nullptr); }

    bool isMaster() const   { return insertID.isEmpty(); }

    te::AudioTrack* getTrack() const
    {
        auto* edit = mixer.state.getEdit();
        return edit != nullptr && ! isMaster() ? findInsert (*edit, insertID) : nullptr;
    }

    te::VolumeAndPanPlugin* getVolume() const
    {
        auto* edit = mixer.state.getEdit();

        if (edit == nullptr)
            return nullptr;

        if (isMaster())
            return edit->getMasterVolumePlugin().get();

        auto* t = getTrack();
        return t != nullptr ? getVolumePlugin (*t) : nullptr;
    }

    void select()
    {
        mixer.state.selectedInsert = insertID;
        mixer.refresh();
    }

    void showInputMenu()
    {
        auto& rec = mixer.state.session.getRecorder();
        const auto inputs = rec.getInputs();
        const auto current = rec.getInputFor (insertID);

        juce::PopupMenu m;
        m.addSectionHeader ("Record from (this PC only)");
        m.addItem (1, "No input", true, current.isEmpty());

        for (int i = 0; i < inputs.size(); ++i)
            m.addItem (100 + i, inputs[i].name, true, inputs[i].id == current);

        if (inputs.isEmpty())
            m.addItem (-1, "(no inputs: choose your interface in Audio settings...)", false);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (input),
                         [self = juce::Component::SafePointer<Strip> (this), inputs] (int r)
        {
            if (self == nullptr || r == 0)
                return;

            auto& recorder = self->mixer.state.session.getRecorder();

            if (r == 1)
                recorder.setInputFor (self->insertID, {});
            else if (juce::isPositiveAndBelow (r - 100, inputs.size()))
                recorder.setInputFor (self->insertID, inputs[r - 100].id);   // also arms it

            self->refresh();
        });
    }

    void showMenu()
    {
        auto* t = getTrack();

        if (t == nullptr)
            return;

        juce::PopupMenu m;
        m.addItem (1, "Rename...");
        m.addItem (2, "Delete insert");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (name),
                         [self = juce::Component::SafePointer<Strip> (this)] (int r)
        {
            if (self == nullptr)
                return;     // the strip was rebuilt while the menu was open

            self->menuChosen (r);
        });
    }

    void menuChosen (int r)
    {
        {
            auto* edit = mixer.state.getEdit();
            auto* track = getTrack();

            if (edit == nullptr || track == nullptr || r == 0)
                return;

            if (r == 1)
            {
                auto* w = new juce::AlertWindow ("Rename insert", {}, juce::MessageBoxIconType::NoIcon);
                w->addTextEditor ("name", track->getName());
                w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                const auto id = insertID;
                auto& st = mixer.state;
                w->enterModalState (true, juce::ModalCallbackFunction::create ([&st, id, w] (int res)
                {
                    if (res == 1 && st.getEdit() != nullptr)
                        if (auto* t2 = findInsert (*st.getEdit(), id))
                        {
                            st.beginAction();
                            t2->setName (w->getTextEditorContents ("name").trim().substring (0, 40));
                        }
                }), true);
            }
            else if (r == 2)
            {
                mixer.state.beginAction();
                mixer.state.selectedInsert = {};
                removeInsert (*edit, *track);
            }
        }
    }

    void refresh()
    {
        auto* edit = mixer.state.getEdit();

        if (edit == nullptr)
            return;

        const bool selected = mixer.state.selectedInsert == insertID;
        auto* t = getTrack();

        if (isMaster())
            name.setButtonText ("Master");
        else if (t != nullptr)
            name.setButtonText (juce::String (getInsertNumber (*edit, *t)) + " " + t->getName());

        name.setColour (juce::TextButton::buttonColourId, selected ? accent.darker (0.2f) : juce::Colour (0xff3a414a));
        name.setColour (juce::TextButton::textColourOffId, selected ? juce::Colours::black : juce::Colours::white);

        if (t != nullptr)
        {
            const bool muted = t->isMuted (false);
            mute.setButtonText (muted ? "Muted" : "On");
            mute.setColour (juce::TextButton::buttonColourId, muted ? juce::Colour (0xff555555) : juce::Colour (0xff3f7a3f));

            auto* dest = resolveRoute (*edit, *t);
            route.setButtonText (dest != nullptr ? "-> " + juce::String (getInsertNumber (*edit, *dest)) : juce::String ("-> Master"));

            // This PC's recording input (not shared: everyone records their own mic).
            auto& rec = mixer.state.session.getRecorder();
            const auto inputID = rec.getInputFor (insertID);
            juce::String inputName ("No input");

            for (auto& in : rec.getInputs())
                if (in.id == inputID)
                    inputName = in.name;

            const bool armed = rec.isArmed (insertID);
            input.setButtonText (inputName);
            input.setColour (juce::TextButton::textColourOffId, inputID.isEmpty() ? juce::Colours::grey : juce::Colours::white);
            arm.setColour (juce::TextButton::buttonColourId, armed ? juce::Colour (0xffd03a3a) : juce::Colour (0xff3a414a));
            arm.setColour (juce::TextButton::textColourOffId, armed ? juce::Colours::white : juce::Colour (0xffd06060));
        }

        if (auto* v = getVolume())
        {
            if (! fader.isMouseButtonDown())
                fader.setValue (v->volParam->getCurrentNormalisedValue(), juce::dontSendNotification);

            if (! pan.isMouseButtonDown())
                pan.setValue (v->panParam->getCurrentNormalisedValue(), juce::dontSendNotification);

            auto range = v->volParam->getValueRange();
            // Back to where it starts: 0 dB on inserts, -3 dB (headroom) on the master.
            fader.setDoubleClickReturnValue (true, (te::decibelsToVolumeFaderPosition (isMaster() ? -3.0f : 0.0f) - range.getStart())
                                                     / juce::jmax (0.0001f, range.getLength()));
            const float db = te::volumeFaderPositionToDB (v->volParam->getCurrentValue());
            dbLabel.setText (db <= -96.0f ? juce::String ("-inf") : juce::String (db, 1) + " dB", juce::dontSendNotification);
        }

        repaint();
    }

    void updateMeter()
    {
        te::LevelMeasurer* measurer = nullptr;

        if (isMaster())
        {
            if (auto* edit = mixer.state.getEdit())
                if (auto* ctx = edit->getTransport().getCurrentPlaybackContext())
                    measurer = &ctx->masterLevels;
        }
        else if (auto* t = getTrack())
        {
            if (auto* lm = t->getLevelMeterPlugin())
                measurer = &lm->measurer;
        }

        meter.update (measurer);
    }

    void paint (juce::Graphics& g) override
    {
        const bool selected = mixer.state.selectedInsert == insertID;
        g.setColour (selected ? selectedStrip : stripColour);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f, 0.0f), 4.0f);
    }

    void mouseDown (const juce::MouseEvent&) override   { select(); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 4);
        name.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);

        // Input + arm row (kept empty on the master so faders line up).
        auto inputRow = r.removeFromTop (22);
        r.removeFromTop (4);

        if (! isMaster())
        {
            arm.setBounds (inputRow.removeFromRight (24));
            inputRow.removeFromRight (3);
            input.setBounds (inputRow);
        }

        if (! isMaster())
        {
            route.setBounds (r.removeFromBottom (22));
            r.removeFromBottom (3);
            mute.setBounds (r.removeFromBottom (22));
            r.removeFromBottom (3);
        }

        pan.setBounds (r.removeFromBottom (32).withSizeKeepingCentre (32, 32));
        dbLabel.setBounds (r.removeFromBottom (16));
        meter.setBounds (r.removeFromRight (10).reduced (0, 4));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

    Mixer& mixer;
    const juce::String insertID;
    juce::TextButton name, mute, route, input, arm;
    MeterBar meter;
    ResetSlider fader, pan;
    juce::Label dbLabel;
};

//==============================================================================
class Mixer::Slots  : public juce::Component
{
public:
    explicit Slots (Mixer& m) : mixer (m)
    {
        title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (title);

        for (int i = 0; i < numSlots; ++i)
        {
            auto* b = buttons.add (new juce::TextButton());
            b->onClick = [this, i] { slotClicked (i); };
            addAndMakeVisible (b);
        }
    }

    te::AudioTrack* getInsert() const
    {
        auto* edit = mixer.state.getEdit();
        return edit != nullptr ? findInsert (*edit, mixer.state.selectedInsert) : nullptr;
    }

    juce::Array<te::Plugin*> getChain() const
    {
        auto* edit = mixer.state.getEdit();

        if (edit == nullptr)
            return {};

        if (auto* t = getInsert())
            return getInsertEffects (*t);

        return getMasterEffects (*edit);
    }

    void refresh()
    {
        auto* edit = mixer.state.getEdit();

        if (edit == nullptr)
            return;

        if (! mixer.state.selectedInsert.isEmpty() && getInsert() == nullptr)
            mixer.state.selectedInsert = {};   // it was deleted

        auto* t = getInsert();
        title.setText (t != nullptr ? "Insert " + juce::String (getInsertNumber (*edit, *t)) + " - " + t->getName() + ": effects"
                                    : juce::String ("Master: effects"),
                       juce::dontSendNotification);

        auto chain = getChain();

        for (int i = 0; i < numSlots; ++i)
        {
            auto* b = buttons[i];

            if (i < chain.size())
            {
                auto* p = chain[i];
                auto* ext = dynamic_cast<te::ExternalPlugin*> (p);
                const bool missing = ext != nullptr && ext->getAudioPluginInstance() == nullptr;
                b->setButtonText (juce::String (i + 1) + ". " + p->getName()
                                    + (missing ? "  (not installed here)" : ! p->isEnabled() ? "  (bypassed)" : ""));
                b->setColour (juce::TextButton::buttonColourId, missing ? juce::Colours::darkred
                                                                : p->isEnabled() ? juce::Colour (0xff3f4a57) : juce::Colour (0xff2c3138));
                b->setTooltip ("Click: open. Right-click: bypass, move, replace, remove");
            }
            else
            {
                b->setButtonText (juce::String (i + 1) + ".  + add effect");
                b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff262b32));
                b->setTooltip ("Click to add an effect here");
            }
        }
    }

    void slotClicked (int index)
    {
        auto* edit = mixer.state.getEdit();

        if (edit == nullptr)
            return;

        auto chain = getChain();
        auto* button = buttons[index];

        if (index >= chain.size())
        {
            const auto insertID = mixer.state.selectedInsert;
            auto& st = mixer.state;

            Mixer::showEffectMenu (mixer.engine, button, [&st, insertID] (const juce::String& type, const juce::PluginDescription* desc)
            {
                if (auto* e = st.getEdit())
                {
                    st.beginAction();
                    putEffect (*e, findInsert (*e, insertID), createEffect (*e, type, desc), -1);
                }
            });
            return;
        }

        te::Plugin::Ptr plugin (chain[index]);

        if (! juce::ModifierKeys::currentModifiers.isPopupMenu())
        {
            plugin->showWindowExplicitly();
            return;
        }

        juce::PopupMenu m;
        m.addItem (1, "Open");
        m.addItem (2, plugin->isEnabled() ? "Bypass" : "Enable");
        m.addItem (3, "Move up", index > 0);
        m.addItem (4, "Move down", index < chain.size() - 1);
        m.addItem (5, "Replace...");
        m.addItem (6, "Remove");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (button), [this, plugin, index, button] (int r)
        {
            auto* e = mixer.state.getEdit();

            if (e == nullptr || r == 0 || &plugin->edit != e)
                return;

            if (r == 1) { plugin->showWindowExplicitly(); return; }

            mixer.state.beginAction();

            if (r == 2)      plugin->setEnabled (! plugin->isEnabled());
            else if (r == 3) moveEffect (*plugin, -1);
            else if (r == 4) moveEffect (*plugin, 1);
            else if (r == 6) plugin->deleteFromParent();
            else if (r == 5)
            {
                const auto insertID = mixer.state.selectedInsert;
                auto& st = mixer.state;

                Mixer::showEffectMenu (mixer.engine, button, [&st, insertID, plugin, index] (const juce::String& type,
                                                                                         const juce::PluginDescription* desc)
                {
                    auto* e2 = st.getEdit();

                    if (e2 != nullptr && &plugin->edit == e2)   // the song may have been replaced meanwhile
                    {
                        st.beginAction();
                        plugin->deleteFromParent();
                        putEffect (*e2, findInsert (*e2, insertID), createEffect (*e2, type, desc), index);
                    }
                });
            }

            mixer.refresh();
        });
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff23272d));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        const int h = juce::jlimit (18, 26, r.getHeight() / numSlots - 3);

        for (auto* b : buttons)
        {
            b->setBounds (r.removeFromTop (h));
            r.removeFromTop (3);
        }
    }

    Mixer& mixer;
    juce::Label title;
    juce::OwnedArray<juce::TextButton> buttons;
};

//==============================================================================
Mixer::Mixer (te::Engine& e, EditorState& s) : engine (e), state (s)
{
    slots = std::make_unique<Slots> (*this);
    stripsViewport.setViewedComponent (&stripsContent, false);
    stripsViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripsViewport);
    addAndMakeVisible (*slots);
    stripsContent.addAndMakeVisible (addInsertButton);

    addInsertButton.onClick = [this]
    {
        if (auto* edit = state.getEdit())
        {
            state.beginAction();

            if (auto* t = addInsert (*edit, {}))
                state.selectedInsert = t->itemID.toString();
        }
    };

    startTimerHz (30);
}

Mixer::~Mixer()
{
    stopTimer();
}

void Mixer::refresh()
{
    auto* edit = state.getEdit();
    juce::StringArray ids;
    ids.add ({});   // master

    if (edit != nullptr)
        for (auto* t : getInserts (*edit))
            ids.add (t->itemID.toString());

    if (ids != stripIDs)
    {
        strips.clear();
        stripIDs = ids;

        for (auto& id : ids)
            stripsContent.addAndMakeVisible (strips.add (new Strip (*this, id)));

        layoutStrips();
    }

    for (auto* s : strips)
        s->refresh();

    slots->refresh();
}

void Mixer::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void Mixer::resized()
{
    auto r = getLocalBounds().reduced (4);
    slots->setBounds (r.removeFromRight (slotsWidth));
    r.removeFromRight (6);
    stripsViewport.setBounds (r);
    layoutStrips();
}

void Mixer::layoutStrips()
{
    const int h = juce::jmax (100, stripsViewport.getHeight() - stripsViewport.getScrollBarThickness());
    stripsContent.setSize (strips.size() * stripWidth + 100, h);

    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * stripWidth + (i > 0 ? 8 : 0), 0, stripWidth, h);

    addInsertButton.setBounds (strips.size() * stripWidth + 14, 4, 80, 26);
}

void Mixer::timerCallback()
{
    for (auto* s : strips)
        s->updateMeter();

    static int tick = 0;

    if (++tick % 6 == 0)    // follow changes made by other people
        for (auto* s : strips)
            s->refresh();
}

//==============================================================================
void Mixer::showEffectMenu (te::Engine& engine, juce::Component* target, EffectChoice onChosen)
{
    juce::PopupMenu m, builtIn, vst;
    auto builtIns = getBuiltInEffects();

    for (int i = 0; i < builtIns.size(); ++i)
        builtIn.addItem (1 + i, builtIns[i].second);

    juce::Array<juce::PluginDescription> types;

    for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
        if (! d.isInstrument)
            types.add (d);

    if (types.isEmpty())
        vst.addItem (-1, "(none found - use Scan plugins...)", false);
    else
        juce::KnownPluginList::addToMenu (vst, types, juce::KnownPluginList::sortByManufacturer);

    m.addSubMenu ("Your plugins (VST3: Waves, FabFilter...)", vst);
    m.addSubMenu ("Built-in", builtIn);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [builtIns, types, onChosen] (int result)
    {
        if (result == 0 || ! onChosen)
            return;

        if (result >= 1 && result <= builtIns.size())
        {
            onChosen (builtIns[result - 1].first, nullptr);
            return;
        }

        auto index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

        if (juce::isPositiveAndBelow (index, types.size()))
            onChosen ({}, &types.getReference (index));
    });
}

void Mixer::showRouteMenu (EditorState& state, te::AudioTrack& source, juce::Component* target)
{
    auto* edit = state.getEdit();

    if (edit == nullptr)
        return;

    juce::PopupMenu m;
    const auto current = getRoute (source);
    m.addItem (1, "Master", true, resolveRoute (*edit, source) == nullptr);

    auto inserts = getInserts (*edit);

    for (int i = 0; i < inserts.size(); ++i)
    {
        auto* ins = inserts[i];

        if (ins == &source)
            continue;

        const auto id = ins->itemID.toString();
        m.addItem (100 + i, juce::String (i + 1) + "  " + ins->getName(), canRouteTo (*edit, source, id), current == id);
    }

    m.addSeparator();
    m.addItem (2, "New insert for \"" + source.getName() + "\"");

    const auto sourceID = source.itemID.toString();

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target), [&state, sourceID] (int r)
    {
        auto* e = state.getEdit();

        if (e == nullptr || r == 0)
            return;

        te::AudioTrack* src = nullptr;

        for (auto* t : te::getAudioTracks (*e))
            if (t->itemID.toString() == sourceID)
                src = t;

        if (src == nullptr)
            return;

        state.beginAction();

        if (r == 1)
        {
            setRoute (*e, *src, {});
        }
        else if (r == 2)
        {
            if (auto* ins = addInsert (*e, src->getName()))
            {
                setRoute (*e, *src, ins->itemID.toString());
                state.selectedInsert = ins->itemID.toString();
            }
        }
        else if (auto* ins = getInserts (*e)[r - 100])
        {
            setRoute (*e, *src, ins->itemID.toString());
        }
    });
}

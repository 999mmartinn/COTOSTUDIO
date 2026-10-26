#include "Mixer.h"
#include "Look.h"

using namespace song;

namespace col = theme::colours;

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

        bool changed = false;

        for (int ch = 0; ch < 2; ++ch)
        {
            const float db = attached != nullptr ? client.getAndClearAudioLevel (ch).dB : -100.0f;

            // Fast attack, slow fall.
            const float target = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
            const float before = level[ch];
            level[ch] = target > level[ch] ? target : juce::jmax (target, level[ch] - 0.025f);
            changed = changed || std::abs (before - level[ch]) > 0.002f;

            const bool wasClipped = clipped[ch];
            clipped[ch] = db > 0.0f || (clipped[ch] && ++clipHold[ch] < 60);

            if (db > 0.0f)
                clipHold[ch] = 0;

            changed = changed || wasClipped != clipped[ch];
        }

        if (changed)
            repaint();   // (a silent meter costs nothing)
    }

    void paint (juce::Graphics& g) override
    {
        // Two bars (left / right): green, amber from -12 dB, a red light when it clips.
        auto r = getLocalBounds().toFloat();
        const float w = (r.getWidth() - 2.0f) * 0.5f;
        const float amberFrom = 48.0f / 66.0f;   // -12 dB

        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = juce::Rectangle<float> (r.getX() + ch * (w + 2.0f), r.getY(), w, r.getHeight());
            g.setColour (theme::colours::stepOff);
            g.fillRoundedRectangle (bar, 2.0f);

            auto inner = bar.withTrimmedTop (5.0f);
            const float h = inner.getHeight();
            const float greenH = h * juce::jmin (level[ch], amberFrom);
            const float amberH = h * juce::jmax (0.0f, level[ch] - amberFrom);

            g.setColour (theme::colours::green);
            g.fillRect (inner.getX(), inner.getBottom() - greenH, inner.getWidth(), greenH);
            g.setColour (theme::colours::amber);
            g.fillRect (inner.getX(), inner.getBottom() - greenH - amberH, inner.getWidth(), amberH);

            g.setColour (clipped[ch] ? theme::colours::red : theme::colours::stepOffAlt);
            g.fillRoundedRectangle (bar.withHeight (4.0f), 1.5f);
        }
    }

private:
    te::LevelMeasurer::Client client;
    juce::WeakReference<te::LevelMeasurer> attached;
    float level[2] = { 0.0f, 0.0f };
    bool clipped[2] = { false, false };
    int clipHold[2] = { 0, 0 };

    void detach()
    {
        if (auto* m = attached.get())
            m->removeClient (client);

        attached = nullptr;
    }
};

//==============================================================================
/** The sends of one insert: a level knob per send, remove buttons, "+ Send to...". */
class SendsPanel  : public juce::Component,
                    private juce::Timer
{
public:
    SendsPanel (EditorState& s, const juce::String& id) : state (s), insertID (id)
    {
        title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (title);
        addButton.setTooltip (TRANS ("Feed another insert from this one too (e.g. a reverb or delay insert)"));
        addButton.onClick = [this] { showAddMenu(); };
        addAndMakeVisible (addButton);
        rebuild();
        startTimerHz (8);
    }

    ~SendsPanel() override
    {
        for (auto* r : rows)
            r->knob.setLookAndFeel (nullptr);
    }

private:
    struct Row
    {
        juce::String sid;
        juce::Label target, value;
        ResetSlider knob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
        juce::TextButton remove { juce::String::fromUTF8 ("\xc3\x97") };
    };

    EditorState& state;
    const juce::String insertID;
    juce::Label title;
    juce::TextButton addButton { "+ Send to..." };
    juce::OwnedArray<Row> rows;
    juce::StringArray shownSids;

    te::Edit* edit() const     { return state.getEdit(); }

    juce::ValueTree findSend (const juce::String& sid) const
    {
        if (auto* e = edit())
            for (auto s : getSendsFrom (*e, insertID))
                if (s[ids::sid].toString() == sid)
                    return s;

        return {};
    }

    static double positionToDb (double pos)
    {
        const double db = te::volumeFaderPositionToDB ((float) pos);
        return db <= minSendDb ? minSendDb : juce::jmin (maxSendDb, db);
    }

    static juce::String dbText (double db)
    {
        return db <= minSendDb ? juce::String ("off") : (db > 0 ? "+" : "") + juce::String (db, 1) + " dB";
    }

    void rebuild()
    {
        for (auto* r : rows)
            r->knob.setLookAndFeel (nullptr);

        rows.clear();
        shownSids.clear();
        auto* e = edit();
        auto* insert = e != nullptr ? findInsert (*e, insertID) : nullptr;

        if (insert == nullptr)
            return;

        title.setText (TRANS ("Sends from") + " " + juce::String (getInsertNumber (*e, *insert)) + " " + insert->getName(), juce::dontSendNotification);

        for (auto s : getSendsFrom (*e, insertID))
        {
            auto* row = rows.add (new Row());
            row->sid = s[ids::sid].toString();
            shownSids.add (row->sid);

            auto* target = findInsert (*e, s[ids::to].toString());
            row->target.setText (target != nullptr ? "-> " + juce::String (getInsertNumber (*e, *target)) + " " + target->getName()
                                                   : juce::String ("-> (gone)"), juce::dontSendNotification);

            auto& k = row->knob;
            k.setRange (0.0, 1.0);
            k.setLookAndFeel (&knobLook());
            k.setColour (juce::Slider::rotarySliderFillColourId, col::accent);
            k.setDoubleClickReturnValue (true, te::decibelsToVolumeFaderPosition (0.0f));
            k.describeDefault = [] { return juce::String ("0 dB"); };
            k.setTooltip (TRANS ("Send level (right-click: reset to 0 dB). All the way down = off"));
            k.setValue (te::decibelsToVolumeFaderPosition ((float) getSendLevel (s)), juce::dontSendNotification);
            k.onDragStart = [this] { state.beginAction(); };
            k.onValueChange = [this, row]
            {
                auto send = findSend (row->sid);

                if (auto* e2 = edit(); e2 != nullptr && send.isValid())
                    setSendLevel (*e2, send, positionToDb (row->knob.getValue()));

                row->value.setText (dbText (positionToDb (row->knob.getValue())), juce::dontSendNotification);
            };

            row->value.setText (dbText (getSendLevel (s)), juce::dontSendNotification);
            row->value.setJustificationType (juce::Justification::centredRight);
            row->remove.setTooltip (TRANS ("Remove this send"));
            row->remove.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff5a3030));
            row->remove.onClick = [this, sid = row->sid]
            {
                auto send = findSend (sid);

                if (auto* e2 = edit(); e2 != nullptr && send.isValid())
                {
                    state.beginAction();
                    removeSend (*e2, send);
                }

                juce::MessageManager::callAsync ([self = juce::Component::SafePointer<SendsPanel> (this)]
                                                 { if (self != nullptr) self->rebuild(); });
            };

            for (auto* c : std::initializer_list<juce::Component*> { &row->target, &row->value, &row->knob, &row->remove })
                addAndMakeVisible (c);
        }

        setSize (330, 38 + rows.size() * 40 + 34);
        resized();
    }

    void showAddMenu()
    {
        auto* e = edit();

        if (e == nullptr)
            return;

        juce::PopupMenu m;
        m.addSectionHeader (TRANS ("Send to"));
        juce::StringArray targets;

        for (auto* t : getInserts (*e))
        {
            const auto id = t->itemID.toString();

            if (id == insertID)
                continue;

            targets.add (id);
            const bool ok = canSendTo (*e, insertID, id);
            m.addItem (targets.size(), juce::String (getInsertNumber (*e, *t)) + " " + t->getName()
                                         + (ok ? juce::String() : "  (would loop, or already sent)"), ok);
        }

        if (targets.isEmpty())
            m.addItem (-1, TRANS ("(make another insert first: + Insert)"), false);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton),
                         [self = juce::Component::SafePointer<SendsPanel> (this), targets] (int r)
        {
            if (self == nullptr || ! juce::isPositiveAndBelow (r - 1, targets.size()))
                return;

            if (auto* e2 = self->edit())
            {
                self->state.beginAction();
                addSend (*e2, self->insertID, targets[r - 1], 0.0);
            }

            self->rebuild();
        });
    }

    void timerCallback() override
    {
        auto* e = edit();

        if (e == nullptr || findInsert (*e, insertID) == nullptr)
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();   // the insert (or the song) went away

            return;
        }

        juce::StringArray sids;

        for (auto s : getSendsFrom (*e, insertID))
            sids.add (s[ids::sid].toString());

        if (sids != shownSids)
        {
            rebuild();   // someone added or removed a send
            return;
        }

        for (auto* row : rows)
        {
            auto send = findSend (row->sid);

            if (send.isValid() && ! row->knob.isMouseButtonDown())
            {
                row->knob.setValue (te::decibelsToVolumeFaderPosition ((float) getSendLevel (send)), juce::dontSendNotification);
                row->value.setText (dbText (getSendLevel (send)), juce::dontSendNotification);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);

        for (auto* row : rows)
        {
            auto line = r.removeFromTop (36);
            row->remove.setBounds (line.removeFromRight (26).withSizeKeepingCentre (26, 24));
            line.removeFromRight (4);
            row->value.setBounds (line.removeFromRight (64));
            row->knob.setBounds (line.removeFromRight (36));
            row->target.setBounds (line);
            r.removeFromTop (4);
        }

        addButton.setBounds (r.removeFromTop (26).removeFromLeft (120));
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
        fader.setColour (juce::Slider::trackColourId, col::accent);
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
        pan.setColour (juce::Slider::rotarySliderFillColourId, col::lilac);
        pan.setColour (juce::Slider::rotarySliderOutlineColourId, col::stepOffAlt);
        pan.getProperties().set ("bipolar", true);
        pan.setLookAndFeel (&knobLook());
        pan.setTooltip (TRANS ("Pan (right-click: reset to centre)"));
        pan.describeDefault = [] { return TRANS ("centre"); };
        fader.setTooltip (isMaster() ? TRANS ("Volume (right-click: reset to -3 dB)") : TRANS ("Volume (right-click: reset to 0 dB)"));
        fader.describeDefault = [this] { return juce::String (isMaster() ? "-3 dB" : "0 dB"); };
        pan.onDragStart = [this] { mixer.state.beginAction(); };
        mixer.state.enableAutomation (fader, [this] { return (te::Plugin*) getVolume(); }, "volume");
        mixer.state.enableAutomation (pan, [this] { return (te::Plugin*) getVolume(); }, "pan");
        pan.onValueChange = [this]
        {
            if (auto* v = getVolume())
                v->panParam->setNormalisedParameter ((float) pan.getValue(), juce::sendNotification);
        };

        name.onClick = [this]
        {
            select();

            if (name.wasRightClick())
                showMenu();
        };

        mute.setButtonText ("M");
        mute.setTooltip (TRANS ("Mute (for everyone)"));
        mute.onClick = [this]
        {
            if (auto* t = getTrack())
            {
                mixer.state.beginAction();
                t->setMute (! t->isMuted (false));
            }
        };

        solo.setButtonText ("S");
        solo.setTooltip (TRANS ("Solo: hear only this insert (and what flows into and out of it). Only on your PC - people following your share hear it too"));
        solo.onClick = [this]
        {
            auto& session = mixer.state.session;

            if (! session.toggleSolo (insertID, true))
                session.logMessage (session.getShareOwnerName() + " controls what you hear right now (leave their share to solo)");

            mixer.refresh();
        };

        sends.setTooltip (TRANS ("Sends: also feed other inserts from this one, each at its own level (e.g. a reverb insert)"));
        sends.onClick = [this] { showSends(); };

        input.setTooltip (TRANS ("Audio input recorded into this insert (your interface / mic). You hear it through this insert's effects"));
        input.onClick = [this] { showInputMenu(); };
        arm.setTooltip (TRANS ("Arm for recording: the Record button records this insert's input"));
        theme::setIcon (arm, theme::Icon::record);
        theme::setStyle (arm, "record");
        arm.onClick = [this]
        {
            auto& rec = mixer.state.session.getRecorder();

            if (rec.getInputFor (insertID).isEmpty())
                showInputMenu();
            else
                rec.setArmed (insertID, ! rec.isArmed (insertID));

            refresh();
        };

        route.setTooltip (TRANS ("Where this insert's sound goes"));
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
            addAndMakeVisible (solo);
            addAndMakeVisible (route);
            addAndMakeVisible (sends);
            addAndMakeVisible (input);
            addAndMakeVisible (arm);
        }

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (theme::mono (10.5f));
        dbLabel.setColour (juce::Label::textColourId, col::text);
        dbLabel.setInterceptsMouseClicks (false, false);

        // The name plate is painted (coloured); the button on it only takes clicks.
        name.setAlpha (0.0f);

        for (auto* b : { &mute, &solo, &sends, &route, &input, &arm })
            b->getProperties().set ("small", true);
    }

    juce::Colour getColour() const
    {
        auto* edit = mixer.state.getEdit();
        auto* t = getTrack();

        if (isMaster() || edit == nullptr || t == nullptr)
            return col::accent;

        return theme::channelColour (getInsertNumber (*edit, *t) - 1);
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
        m.addSectionHeader (TRANS ("Record from (this PC only)"));
        m.addItem (1, TRANS ("No input"), true, current.isEmpty());

        for (int i = 0; i < inputs.size(); ++i)
            m.addItem (100 + i, inputs[i].name, true, inputs[i].id == current);

        if (inputs.isEmpty())
            m.addItem (-1, TRANS ("(no inputs: choose your interface in Audio settings...)"), false);

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
        m.addItem (1, TRANS ("Rename..."));
        m.addItem (2, TRANS ("Delete insert"));

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
                auto* w = new juce::AlertWindow (TRANS ("Rename insert"), {}, juce::MessageBoxIconType::NoIcon);
                w->addTextEditor ("name", track->getName());
                w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
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
            plateText = TRANS ("Master");
        else if (t != nullptr)
            plateText = juce::String (getInsertNumber (*edit, *t)) + " " + t->getName();

        name.setTooltip (plateText + "  -  " + TRANS ("click: select (its effects on the right). Right-click: rename, delete"));
        juce::ignoreUnused (selected);

        // Effects shown on the strip (Pro Tools-style inserts).
        effectNames.clearQuick();
        effectStates.clearQuick();

        for (auto* p : (t != nullptr ? getInsertEffects (*t) : getMasterEffects (*edit)))
        {
            auto* ext = dynamic_cast<te::ExternalPlugin*> (p);
            effectNames.add (p->getName());
            effectStates.add (ext != nullptr && ext->getAudioPluginInstance() == nullptr ? 2 : p->isEnabled() ? 0 : 1);
        }

        if (t != nullptr)
        {
            const bool muted = t->isMuted (false);
            mute.setToggleState (muted, juce::dontSendNotification);
            mute.setColour (juce::TextButton::buttonOnColourId, col::muted);
            mute.setColour (juce::TextButton::textColourOnId, col::accentText);

            auto* dest = resolveRoute (*edit, *t);
            route.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x92 ") + (dest != nullptr ? juce::String (getInsertNumber (*edit, *dest)) + " " + dest->getName()
                                                                                       : TRANS ("Master")));

            const int numSends = getSendsFrom (*edit, insertID).size();
            sends.setButtonText (numSends > 0 ? TRANS ("Sends") + " (" + juce::String (numSends) + ")" : TRANS ("Sends"));
            sends.setToggleState (numSends > 0, juce::dontSendNotification);

            // Solo is personal: yellow = soloed here; the strip fades when a solo elsewhere silences it.
            const bool soloed = mixer.state.session.isSoloed (insertID);
            solo.setToggleState (soloed, juce::dontSendNotification);
            solo.setColour (juce::TextButton::buttonOnColourId, col::amber);
            solo.setColour (juce::TextButton::textColourOnId, col::accentText);
            setAlpha (mixer.state.session.isSilencedBySolo (insertID) || muted ? 0.6f : 1.0f);

            // This PC's recording input (not shared: everyone records their own mic).
            auto& rec = mixer.state.session.getRecorder();
            const auto inputID = rec.getInputFor (insertID);
            juce::String inputName (TRANS ("No input"));

            for (auto& in : rec.getInputs())
                if (in.id == inputID)
                    inputName = in.name;

            const bool armed = rec.isArmed (insertID);
            input.setButtonText (inputName);
            input.setColour (juce::TextButton::textColourOffId, inputID.isEmpty() ? col::dim : col::text);
            arm.setToggleState (armed, juce::dontSendNotification);
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
            dbLabel.setText (db <= -96.0f ? juce::String::fromUTF8 ("-\xe2\x88\x9e") : (db > 0.05f ? "+" : "") + juce::String (db, 1), juce::dontSendNotification);
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
        auto box = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (selected ? col::panel : col::well);
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (selected ? col::accent : col::line);
        g.drawRoundedRectangle (box, 8.0f, 1.0f);

        // Name plate in the strip's colour.
        g.setColour (getColour());
        g.fillRoundedRectangle (plateArea.toFloat(), 5.0f);
        g.setColour (col::accentText);
        g.setFont (theme::ui (11.0f, theme::Weight::bold));
        g.drawFittedText (plateText, plateArea.reduced (4, 0), juce::Justification::centred, 1, 0.8f);

        // Inserts (effects): click one to open it, the empty one to add.
        for (int i = 0; i < numEffectRows; ++i)
        {
            auto row = effectRow (i).toFloat();

            if (row.isEmpty())
                break;

            if (i < effectNames.size())
            {
                const int st = effectStates[i];
                g.setColour (col::wellAlt);
                g.fillRoundedRectangle (row, 3.0f);
                g.setColour (st == 2 ? col::red : col::line);
                g.drawRoundedRectangle (row.reduced (0.5f), 3.0f, 1.0f);
                auto textArea = row.reduced (5.0f, 0.0f);

                // More effects than rows: the last row says how many aren't shown ("+2").
                if (i == numEffectRows - 1 && effectNames.size() > numEffectRows)
                {
                    g.setColour (col::accent);
                    g.setFont (theme::ui (9.5f, theme::Weight::bold));
                    g.drawText ("+" + juce::String (effectNames.size() - numEffectRows), textArea.removeFromRight (18.0f),
                                juce::Justification::centredRight, false);
                }

                g.setColour (st == 0 ? col::text : st == 1 ? col::dim : col::red);
                g.setFont (theme::ui (9.5f, theme::Weight::medium));
                g.drawText (effectNames[i], textArea, juce::Justification::centredLeft, true);
            }
            else
            {
                juce::Path outline, dashed;
                outline.addRoundedRectangle (row.reduced (0.5f), 3.0f);
                const float dashes[] = { 3.0f, 2.5f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
                g.setColour (col::line);
                g.fillPath (dashed);
                g.setColour (col::muted);
                g.setFont (theme::ui (9.5f));
                g.drawText ("+ " + TRANS ("effect"), row.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
                break;
            }
        }

        // dB scale next to the fader.
        auto f = fader.getBounds();

        if (f.getHeight() > 50)
        {
            g.setFont (theme::mono (8.5f));
            g.setColour (col::muted);
            const float thumb = 14.0f;
            const float top = (float) f.getY() + thumb, bottom = (float) f.getBottom() - thumb;

            // From the top; a mark is skipped when it would touch the last one drawn.
            float lastY = -100.0f;

            for (auto db : { 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
            {
                const float pos = te::decibelsToVolumeFaderPosition (db);
                const float y = bottom - pos * (bottom - top);

                if (y - lastY < 11.0f || bottom - y < 11.0f)
                    continue;

                g.drawText (db > 0.0f ? "+" + juce::String ((int) db) : juce::String ((int) db),
                            juce::Rectangle<float> ((float) scaleArea.getX(), y - 5.0f, (float) scaleArea.getWidth(), 10.0f),
                            juce::Justification::centredRight, false);
                lastY = y;
            }

            g.drawText (juce::String::fromUTF8 ("-\xe2\x88\x9e"),
                        juce::Rectangle<float> ((float) scaleArea.getX(), bottom - 5.0f, (float) scaleArea.getWidth(), 10.0f),
                        juce::Justification::centredRight, false);
        }
    }

    juce::Rectangle<int> effectRow (int i) const
    {
        const int h = 17, gap = 2;

        if (effectsArea.getHeight() < (i + 1) * (h + gap) - gap)
            return {};

        return effectsArea.withHeight (h).translated (0, i * (h + gap));
    }

    void mouseDown (const juce::MouseEvent&) override   { select(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown() || ! e.mods.isLeftButtonDown())
            return;

        for (int i = 0; i < numEffectRows; ++i)
        {
            if (! effectRow (i).contains (e.getPosition()))
                continue;

            auto* edit = mixer.state.getEdit();

            if (edit == nullptr)
                return;

            auto* t = getTrack();
            auto chain = t != nullptr ? getInsertEffects (*t) : getMasterEffects (*edit);

            if (i < chain.size())
            {
                chain[i]->showWindowExplicitly();
            }
            else if (i >= chain.size())
            {
                auto& st = mixer.state;
                const auto id = insertID;

                Mixer::showEffectMenu (mixer.engine, this, [&st, id] (const juce::String& type, const juce::PluginDescription* desc)
                {
                    if (auto* e2 = st.getEdit())
                    {
                        st.beginAction();
                        putEffect (*e2, findInsert (*e2, id), createEffect (*e2, type, desc), -1);
                    }
                });
            }

            return;
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        const int total = getHeight();
        plateArea = r.removeFromTop (22);
        name.setBounds (plateArea);
        r.removeFromTop (5);

        // Input + arm (kept empty on the master so faders line up).
        auto inputRow = r.removeFromTop (20);
        r.removeFromTop (5);

        if (! isMaster())
        {
            arm.setBounds (inputRow.removeFromRight (22));
            inputRow.removeFromRight (3);
            input.setBounds (inputRow);
        }

        // Inserts (effects): more rows on taller strips (e.g. the mixer full screen).
        numEffectRows = total >= 620 ? 6 : total >= 520 ? 5 : total >= 440 ? 4 : total >= 360 ? 3 : total >= 300 ? 2 : total >= 250 ? 1 : 0;
        effectsArea = r.removeFromTop (numEffectRows * 19);

        if (numEffectRows > 0)
            r.removeFromTop (4);

        pan.setBounds (r.removeFromTop (28).withSizeKeepingCentre (28, 28));
        r.removeFromTop (4);

        // Bottom: Sends | output, M | S, the dB value (empty on the master, so faders line up).
        auto outRow = r.removeFromBottom (19);
        r.removeFromBottom (3);
        auto muteRow = r.removeFromBottom (20);
        r.removeFromBottom (3);

        if (! isMaster())
        {
            sends.setBounds (outRow.removeFromLeft ((outRow.getWidth() - 3) * 2 / 5));
            outRow.removeFromLeft (3);
            route.setBounds (outRow);
            mute.setBounds (muteRow.removeFromLeft ((muteRow.getWidth() - 3) / 2));
            muteRow.removeFromLeft (3);
            solo.setBounds (muteRow);
        }

        dbLabel.setBounds (r.removeFromBottom (15));
        r.removeFromBottom (2);

        // Fader with its scale on the left and the stereo meter on the right.
        meter.setBounds (r.removeFromRight (12).reduced (0, 2));
        r.removeFromRight (4);
        scaleArea = r.removeFromLeft (juce::jmin (22, r.getWidth() / 3));
        fader.setBounds (r);
    }

    Mixer& mixer;
    const juce::String insertID;
    MenuButton name;
    juce::TextButton mute, solo, route, sends, input, arm;

    void showSends()
    {
        if (getTrack() == nullptr)
            return;

        auto panel = std::make_unique<SendsPanel> (mixer.state, insertID);
        juce::CallOutBox::launchAsynchronously (std::move (panel), sends.getScreenBounds(), nullptr);
    }
    MeterBar meter;
    ResetSlider fader, pan;
    juce::Label dbLabel;
    juce::String plateText;
    juce::StringArray effectNames;
    juce::Array<int> effectStates;   // 0 on, 1 bypassed, 2 not installed here
    juce::Rectangle<int> plateArea, effectsArea, scaleArea;
    int numEffectRows = 0;
};

//==============================================================================
class Mixer::Slots  : public juce::Component
{
public:
    explicit Slots (Mixer& m) : mixer (m)
    {
        title.setFont (theme::ui (12.5f, theme::Weight::bold));
        title.setColour (juce::Label::textColourId, col::text);
        title.setBorderSize ({});
        addAndMakeVisible (title);

        for (int i = 0; i < numSlots; ++i)
        {
            auto* b = buttons.add (new MenuButton());
            b->onClick = [this, i] { slotClicked (i); };
            addAndMakeVisible (b);

            // A visible way to take an effect out of the chain.
            auto* x = removeButtons.add (new juce::TextButton (juce::String::fromUTF8 ("\xc3\x97")));
            x->setTooltip (TRANS ("Remove this effect"));
            theme::setIcon (*x, theme::Icon::close);
            x->setButtonText ({});
            theme::setStyle (*x, "flat");
            x->onClick = [this, i] { removeEffect (i); };
            addChildComponent (x);
        }
    }

    void removeEffect (int index)
    {
        auto* edit = mixer.state.getEdit();
        auto chain = getChain();

        if (edit == nullptr || ! juce::isPositiveAndBelow (index, chain.size()))
            return;

        te::Plugin::Ptr plugin (chain[index]);
        const auto name = plugin->getName();
        mixer.state.beginAction();
        plugin->deleteFromParent();
        mixer.state.session.logMessage ("Removed " + name + " (Ctrl+Z puts it back)");
        mixer.refresh();
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
        title.setText (t != nullptr ? juce::String (getInsertNumber (*edit, *t)) + " " + t->getName()
                                    : TRANS ("Master"),
                       juce::dontSendNotification);
        titleColour = t != nullptr ? theme::channelColour (getInsertNumber (*edit, *t) - 1) : col::accent;
        repaint();

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
                                    + (missing ? "  (" + TRANS ("not installed here") + ")" : ! p->isEnabled() ? "  (" + TRANS ("bypassed") + ")" : juce::String()));
                b->setColour (juce::TextButton::buttonColourId, missing ? col::redSoft : p->isEnabled() ? col::raised : col::well);
                b->setColour (juce::TextButton::textColourOffId, missing ? col::red : p->isEnabled() ? col::text : col::dim);
                b->setTooltip (TRANS ("Click: open. Right-click: bypass, move, replace, remove"));
                removeButtons[i]->setVisible (b->isVisible());
            }
            else
            {
                removeButtons[i]->setVisible (false);
                b->setButtonText (juce::String (i + 1) + ".  + " + TRANS ("add effect"));
                b->setColour (juce::TextButton::buttonColourId, col::wellAlt);
                b->setColour (juce::TextButton::textColourOffId, col::dim);
                b->setTooltip (TRANS ("Click to add an effect here"));
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

        if (! button->wasRightClick())
        {
            plugin->showWindowExplicitly();
            return;
        }

        juce::PopupMenu m;
        m.addItem (1, TRANS ("Open"));
        m.addItem (2, plugin->isEnabled() ? TRANS ("Bypass") : TRANS ("Enable"));
        m.addItem (3, TRANS ("Move up"), index > 0);
        m.addItem (4, TRANS ("Move down"), index < chain.size() - 1);
        m.addItem (5, TRANS ("Replace..."));
        m.addItem (6, TRANS ("Remove"));

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
        auto box = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (col::panel);
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (col::line);
        g.drawRoundedRectangle (box, 8.0f, 1.0f);

        theme::drawHeading (g, getLocalBounds().reduced (10, 0).removeFromTop (20).withTrimmedTop (4), TRANS ("Effects"));
        const auto t = title.getBounds();
        g.setColour (titleColour);
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) t.getX() - 14.0f, (float) t.getCentreY() - 4.0f, 8.0f, 8.0f), 2.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        r.removeFromTop (12);
        title.setBounds (r.removeFromTop (18).withTrimmedLeft (16));
        r.removeFromTop (6);
        const int h = juce::jlimit (18, 26, r.getHeight() / numSlots - 3);

        for (int i = 0; i < buttons.size(); ++i)
        {
            // Slots that don't fit stay hidden (a short dock); the mixer's own window shows them all.
            const bool fits = r.getHeight() >= h;
            auto row = r.removeFromTop (h);
            removeButtons[i]->setBounds (row.removeFromRight (h + 4));
            row.removeFromRight (3);
            buttons[i]->setBounds (row);
            buttons[i]->setVisible (fits);

            if (! fits)
                removeButtons[i]->setVisible (false);

            r.removeFromTop (3);
        }
    }

    Mixer& mixer;
    juce::Label title;
    juce::Colour titleColour { col::accent };
    juce::OwnedArray<MenuButton> buttons;
    juce::OwnedArray<juce::TextButton> removeButtons;
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

    detachButton.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
    fullScreenButton.onClick = [this] { if (onFullScreenToggle) onFullScreenToggle(); };
    fullScreenButton.setTooltip (TRANS ("Fill the whole screen with the mixer (click again to go back)"));
    fullScreenButton.setButtonText (TRANS ("Full screen"));
    theme::setIcon (fullScreenButton, theme::Icon::fullScreen);
    addInsertButton.setButtonText (TRANS ("Insert"));
    theme::setIcon (addInsertButton, theme::Icon::plus);
    addInsertButton.setTooltip (TRANS ("A new mixer insert (route channels into it)"));
    stripsViewport.onScroll = [this] { repaint(); };
    addAndMakeVisible (detachButton);
    addChildComponent (fullScreenButton);
    setDetached (false);

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
    g.fillAll (col::wellAlt);

    // The master strip stands apart from the inserts (a thin line between).
    if (! strips.isEmpty())
    {
        const auto x = stripsViewport.getX() + stripWidth + stripGap / 2 - stripsViewport.getViewPositionX();

        if (x > stripsViewport.getX() && x < stripsViewport.getRight())
        {
            g.setColour (col::line);
            g.fillRect (x, stripsViewport.getY() + 6, 1, stripsViewport.getHeight() - 12 - stripsViewport.getScrollBarThickness());
        }
    }
}

void Mixer::setDetached (bool detached)
{
    detachButton.setButtonText (detached ? TRANS ("Back in the main window") : TRANS ("Own window"));
    theme::setIcon (detachButton, detached ? theme::Icon::attach : theme::Icon::detach);
    detachButton.setTooltip (detached ? TRANS ("Put the mixer back in the main window (closing its window does the same)")
                                      : TRANS ("Give the mixer a window of its own - e.g. full screen on your second monitor"));
    fullScreenButton.setVisible (detached);
    resized();
}

void Mixer::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto right = r.removeFromRight (slotsWidth);
    auto top = right.removeFromTop (28);
    auto widthFor = [] (juce::TextButton& b)
    {
        return (int) juce::GlyphArrangement::getStringWidth (theme::ui (13.0f, theme::Weight::semibold), b.getButtonText()) + 15 + 6 + 24;
    };

    detachButton.setBounds (top.removeFromRight (widthFor (detachButton)));

    if (fullScreenButton.isVisible())
    {
        top.removeFromRight (6);
        fullScreenButton.setBounds (top.removeFromRight (widthFor (fullScreenButton)));
    }

    right.removeFromTop (8);
    slots->setBounds (right);
    r.removeFromRight (8);
    stripsViewport.setBounds (r);
    layoutStrips();
}

void Mixer::layoutStrips()
{
    const int h = juce::jmax (100, stripsViewport.getHeight() - stripsViewport.getScrollBarThickness());
    stripsContent.setSize (strips.size() * (stripWidth + stripGap) + 130, h);

    // Master first, a gap, then the inserts.
    int x = 0;

    for (int i = 0; i < strips.size(); ++i)
    {
        strips[i]->setBounds (x, 0, stripWidth, h);
        x += stripWidth + (i == 0 ? stripGap * 2 : stripGap);
    }

    addInsertButton.setBounds (x + 4, 0, 100, 30);
    repaint();
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
        builtIn.addItem (1 + i, TRANS (builtIns[i].second));

    juce::Array<juce::PluginDescription> types;

    for (auto& d : engine.getPluginManager().knownPluginList.getTypes())
        if (! d.isInstrument)
            types.add (d);

    if (types.isEmpty())
        vst.addItem (-1, TRANS ("(none found - use Scan plugins...)"), false);
    else
        juce::KnownPluginList::addToMenu (vst, types, juce::KnownPluginList::sortByManufacturer);

    m.addSubMenu (TRANS ("Your plugins (VST3: Waves, FabFilter...)"), vst);
    m.addSubMenu (TRANS ("Built-in"), builtIn);

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
    m.addItem (1, TRANS ("Master"), true, resolveRoute (*edit, source) == nullptr);

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
    m.addItem (2, TRANS ("New insert for") + " \"" + source.getName() + "\"");

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

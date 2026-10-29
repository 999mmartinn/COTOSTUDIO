#include "Browser.h"

using namespace song;

static const juce::Colour rowColour = theme::colours::well;

static juce::Colour patternColourOf (const juce::ValueTree& pattern)
{
    auto c = pattern[ids::colour].toString();
    return c.isNotEmpty() ? juce::Colour::fromString (c) : theme::colours::mint;
}

/** Asks for a line of text (rename dialogs). */
static void askForText (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> onOK)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", initial);
    w->addButton (TRANS ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (TRANS ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, onOK] (int result)
    {
        const auto text = w->getTextEditorContents ("text").trim();

        if (result == 1 && text.isNotEmpty())
            onOK (text);
    }), true);
}

//==============================================================================
class Browser::PatternList  : public juce::Component,
                              private juce::ListBoxModel
{
public:
    explicit PatternList (Browser& b) : owner (b), state (b.state)
    {
        list.setModel (this);
        list.setRowHeight (28);
        list.setOutlineThickness (0);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);

        newButton.setTooltip (TRANS ("Make a new, empty pattern"));
        newButton.setButtonText (TRANS ("New pattern"));
        theme::setIcon (newButton, theme::Icon::plus);
        newButton.onClick = [this]
        {
            if (auto* edit = state.getEdit())
            {
                state.beginAction();
                auto p = addPattern (*edit, nextPatternName (*edit), getBars (state.getPattern()));

                if (owner.onPatternChosen)
                    owner.onPatternChosen (p[ids::sid].toString());
            }
        };
        addAndMakeVisible (newButton);
    }

    void refresh()
    {
        auto* edit = state.getEdit();
        patterns = edit != nullptr ? getPatterns (*edit) : juce::Array<juce::ValueTree>();
        list.updateContent();
        list.repaint();
        repaint (getLocalBounds().removeFromTop (headingHeight));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        newButton.setBounds (r.removeFromBottom (28).reduced (6, 0));
        r.removeFromBottom (6);
        r.removeFromTop (headingHeight);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        theme::drawHeading (g, getLocalBounds().removeFromTop (headingHeight).reduced (10, 0),
                            TRANS ("Patterns") + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (patterns.size()));
    }

    static constexpr int headingHeight = 26;
    int getNumRows() override   { return patterns.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || ! juce::isPositiveAndBelow (row, patterns.size()))
            return;

        auto p = patterns[row];
        const auto sid = p[ids::sid].toString();
        const bool viewed = sid == state.getPattern()[ids::sid].toString();
        const auto colour = patternColourOf (p);
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (6, 2);

        g.setColour (viewed ? theme::colours::accentSoft : rowColour);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        r.removeFromLeft (8);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (10).toFloat().withSizeKeepingCentre (10.0f, 10.0f), 3.0f);
        r.removeFromLeft (6);
        r.removeFromRight (6);

        // Who else is looking at this pattern (their colour).
        bool anyDot = false;

        for (auto& person : state.session.getPeople())
        {
            if (person.isMe || person.viewing != sid)
                continue;

            g.setColour (person.colour);
            g.fillEllipse (r.removeFromRight (12).withSizeKeepingCentre (9, 9).toFloat());
            anyDot = true;
        }

        if (anyDot)
            r.removeFromRight (4);

        g.setColour (theme::colours::muted);
        g.setFont (theme::ui (10.5f));
        g.drawText (juce::String (getPatternChannels (*edit, p).size()) + " ch", r.removeFromRight (34),
                    juce::Justification::centredRight);

        g.setColour (theme::colours::text);
        g.setFont (theme::ui (12.5f, viewed ? theme::Weight::bold : theme::Weight::semibold));
        g.drawText (p[ids::name].toString(), r.reduced (2, 0), juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, patterns.size()))
            return;

        if (e.mods.isPopupMenu())
            showMenu (patterns[row]);
        else if (owner.onPatternChosen)
            owner.onPatternChosen (patterns[row][ids::sid].toString());
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, patterns.size()))
            rename (patterns[row]);
    }

    // Dragging a row onto the playlist places the pattern there.
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override
    {
        if (rows.size() > 0 && juce::isPositiveAndBelow (rows[0], patterns.size()))
            return "pattern:" + patterns[rows[0]][ids::sid].toString();

        return {};
    }

    juce::String getTooltipForRow (int) override
    {
        return "Click: open. Drag onto the playlist to place it. Right-click: rename, clone, split by channel, delete";
    }

private:
    Browser& owner;
    EditorState& state;
    juce::ListBox list;
    juce::TextButton newButton { "+ New pattern" };
    juce::Array<juce::ValueTree> patterns;

    void rename (juce::ValueTree pattern)
    {
        askForText ("Rename pattern", pattern[ids::name].toString(), [this, pattern] (juce::String name) mutable
        {
            if (auto* edit = state.getEdit(); edit != nullptr && pattern.getParent().isValid())
            {
                state.beginAction();
                pattern.setProperty (ids::name, name.substring (0, 40), &edit->getUndoManager());
            }
        });
    }

    void showMenu (juce::ValueTree pattern)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const int numChannels = getPatternChannels (*edit, pattern).size();
        juce::PopupMenu m;
        m.addItem (1, TRANS ("Open"));
        m.addItem (2, TRANS ("Rename..."));
        m.addItem (3, TRANS ("Clone"));
        m.addItem (4, TRANS ("Split by channel") + (numChannels >= 2 ? " (" + juce::String (numChannels) + " " + TRANS ("patterns") + ")" : juce::String()),
                   numChannels >= 2);
        m.addSeparator();
        m.addItem (5, TRANS ("Delete"), getPatterns (*edit).size() > 1);

        m.showMenuAsync (juce::PopupMenu::Options(),
                         [this, pattern, safe = juce::Component::SafePointer<PatternList> (this)] (int r)
        {
            auto* e = safe != nullptr ? state.getEdit() : nullptr;

            if (e == nullptr || r == 0 || ! pattern.getParent().isValid())
                return;

            const auto sid = pattern[ids::sid].toString();

            if (r == 1 && owner.onPatternChosen)
            {
                owner.onPatternChosen (sid);
            }
            else if (r == 2)
            {
                rename (pattern);
            }
            else if (r == 3)
            {
                state.beginAction();
                auto copy = clonePattern (*e, pattern);

                if (owner.onPatternChosen)
                    owner.onPatternChosen (copy[ids::sid].toString());
            }
            else if (r == 4)
            {
                const auto name = pattern[ids::name].toString();
                state.beginAction();
                auto parts = splitPatternByChannel (*e, pattern);

                if (! parts.isEmpty())
                {
                    state.session.logMessage ("Split \"" + name + "\" into " + juce::String (parts.size())
                                               + " patterns, one per channel");

                    if (owner.onPatternChosen)
                        owner.onPatternChosen (parts[0][ids::sid].toString());
                }
            }
            else if (r == 5)
            {
                state.beginAction();
                const bool wasViewed = state.getPattern()[ids::sid].toString() == sid;
                removePattern (*e, pattern);

                if (wasViewed && owner.onPatternChosen && ! getPatterns (*e).isEmpty())
                    owner.onPatternChosen (getPatterns (*e)[0][ids::sid].toString());
            }
        });
    }
};

//==============================================================================
class Browser::AudioList  : public juce::Component,
                            private juce::ListBoxModel
{
public:
    explicit AudioList (Browser& b) : owner (b), state (b.state)
    {
        list.setModel (this);
        list.setRowHeight (36);
        list.setOutlineThickness (0);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);

        importButton.setTooltip (TRANS ("Add audio files (WAV, MP3, FLAC...) to the song"));
        importButton.setButtonText (TRANS ("Import audio..."));
        theme::setIcon (importButton, theme::Icon::plus);
        importButton.onClick = [this] { if (owner.onImportAudio) owner.onImportAudio(); };
        addAndMakeVisible (importButton);

        cleanButton.setButtonText (TRANS ("Delete unused takes..."));
        cleanButton.setTooltip (TRANS ("Deletes every recorded take that has no clip in the playlist, for good (asks first)"));
        theme::setIcon (cleanButton, theme::Icon::close);
        cleanButton.onClick = [this] { if (owner.onDeleteUnusedTakes) owner.onDeleteUnusedTakes(); };
        addAndMakeVisible (cleanButton);
    }

    void refresh()
    {
        auto* edit = state.getEdit();
        assets = edit != nullptr ? getAssets (*edit) : juce::Array<juce::ValueTree>();
        list.updateContent();
        list.repaint();
        repaint (getLocalBounds().removeFromTop (headingHeight));
    }

    bool anyMissing() const
    {
        for (auto& a : assets)
            if (! state.session.getAssetStore().has (a[ids::sid].toString()))
                return true;

        return false;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        cleanButton.setBounds (r.removeFromBottom (28).reduced (6, 0));
        r.removeFromBottom (4);
        importButton.setBounds (r.removeFromBottom (28).reduced (6, 0));
        r.removeFromBottom (6);
        r.removeFromTop (headingHeight);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        theme::drawHeading (g, getLocalBounds().removeFromTop (headingHeight).reduced (10, 0),
                            TRANS ("Audio in the song") + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (assets.size()));
    }

    static constexpr int headingHeight = 26;
    int getNumRows() override   { return assets.size(); }

    static te::AudioTrack* channelFor (te::Edit& edit, const juce::String& hash)
    {
        for (auto* t : getChannels (edit))
            if (isAudioChannel (*t) && getChannelAsset (edit, *t) == hash)
                return t;

        return nullptr;
    }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || ! juce::isPositiveAndBelow (row, assets.size()))
            return;

        auto asset = assets[row];
        const auto hash = asset[ids::sid].toString();
        auto* channel = channelFor (*edit, hash);
        const auto colour = channel != nullptr ? EditorState::channelColour (getChannels (*edit).indexOf (channel))
                                               : theme::colours::dim;
        const bool selected = channel != nullptr && state.placeAudioChannel == getChannelID (*channel);
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (6, 2);

        g.setColour (selected ? theme::colours::accentSoft : rowColour);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        r.removeFromLeft (8);
        theme::drawIcon (g, theme::Icon::wave, r.removeFromLeft (14).toFloat().withSizeKeepingCentre (14.0f, 14.0f), colour);
        r.removeFromLeft (6);

        g.setColour (theme::colours::text);
        g.setFont (theme::ui (12.5f, theme::Weight::semibold));
        g.drawText (asset[ids::name].toString(), r.removeFromTop (r.getHeight() / 2 + 1).reduced (2, 0),
                    juce::Justification::bottomLeft, true);

        // Second line: length, and whether the file is here yet / how often it's used.
        auto& store = state.session.getAssetStore();
        const int uses = getAssetUseCount (*edit, hash);
        juce::String info = juce::String ((double) asset[ids::seconds], 1) + " s  -  ";

        if (! store.has (hash))
        {
            const double progress = store.getProgress (hash);
            info << (progress >= 0.0 ? TRANS ("receiving") + " " + juce::String (juce::roundToInt (progress * 100.0)) + "%"
                                     : TRANS ("waiting for the file..."));
        }
        else
        {
            // Where it's used: playlist clips, and sampler channels in the rack.
            const auto samplers = getSamplerChannelsUsing (*edit, hash);
            juce::StringArray where;

            if (uses > 0)
                where.add (juce::String (uses) + " " + (uses == 1 ? TRANS ("clip") : TRANS ("clips")));

            if (! samplers.isEmpty())
                where.add (TRANS ("rack:") + " " + samplers.joinIntoString (", "));

            info << (where.isEmpty() ? TRANS ("not used") : where.joinIntoString (", "));
        }

        g.setColour (theme::colours::muted);
        g.setFont (theme::ui (10.5f));
        g.drawText (info, r.reduced (2, 0), juce::Justification::topLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        auto* edit = state.getEdit();

        if (edit == nullptr || ! juce::isPositiveAndBelow (row, assets.size()))
            return;

        const auto hash = assets[row][ids::sid].toString();

        if (e.mods.isPopupMenu())
        {
            showMenu (hash);
            return;
        }

        // Clicking selects it: a click in the playlist then places it (like FL).
        if (auto* channel = channelFor (*edit, hash))
        {
            state.selectedChannel = state.placeAudioChannel = getChannelID (*channel);

            if (state.onSelectionChanged)
                state.onSelectionChanged();
        }

        list.repaint();
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, assets.size()))
            rename (assets[row][ids::sid].toString());
    }

    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override
    {
        if (rows.size() > 0 && juce::isPositiveAndBelow (rows[0], assets.size()))
            return "asset:" + assets[rows[0]][ids::sid].toString();

        return {};
    }

    juce::String getTooltipForRow (int) override
    {
        return "Click: select it (then click in the playlist to place it). Drag onto the playlist. "
               "Right-click: rename, delete from the project";
    }

private:
    Browser& owner;
    EditorState& state;
    juce::ListBox list;
    juce::TextButton importButton { "Import audio..." }, cleanButton;
    juce::Array<juce::ValueTree> assets;

    void rename (const juce::String& hash)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        askForText ("Rename audio", getAssetName (*edit, hash), [this, hash] (juce::String name)
        {
            if (auto* e = state.getEdit())
            {
                state.beginAction();
                renameAsset (*e, hash, name);
            }
        });
    }

    void showMenu (const juce::String& hash)
    {
        auto* edit = state.getEdit();

        if (edit == nullptr)
            return;

        const int uses = getAssetUseCount (*edit, hash);
        juce::PopupMenu m;
        m.addItem (1, TRANS ("Rename..."));
        m.addSeparator();
        const int samplers = getSamplerChannelsUsing (*edit, hash).size();
        juce::StringArray what;

        if (uses > 0)
            what.add (juce::String (uses) + (uses == 1 ? " clip" : " clips"));

        if (samplers > 0)
            what.add (juce::String (samplers) + (samplers == 1 ? " sampler channel" : " sampler channels"));

        m.addItem (2, TRANS ("Delete from project") + (what.isEmpty() ? juce::String() : " (" + TRANS ("and its") + " " + what.joinIntoString (", ") + ")"));

        m.showMenuAsync (juce::PopupMenu::Options(),
                         [this, hash, safe = juce::Component::SafePointer<AudioList> (this)] (int r)
        {
            auto* e = safe != nullptr ? state.getEdit() : nullptr;

            if (e == nullptr || r == 0)
                return;

            if (r == 1)
            {
                rename (hash);
            }
            else if (r == 2)
            {
                const auto name = getAssetName (*e, hash);
                state.beginAction();
                removeAsset (*e, hash);
                state.session.logMessage ("Deleted \"" + name + "\" from the project (Ctrl+Z brings it back)");
            }
        });
    }
};

//==============================================================================
class Browser::SessionPanel  : public juce::Component
{
public:
    explicit SessionPanel (Browser& b) : owner (b), state (b.state)
    {
        nameLabel.setText (TRANS ("Your name"), juce::dontSendNotification);
        nameLabel.setFont (theme::ui (11.5f));
        nameLabel.setColour (juce::Label::textColourId, theme::colours::muted);
        addAndMakeVisible (nameLabel);

        nameEditor.setText (state.session.getUserName(), false);
        nameEditor.setTooltip (TRANS ("How the others see you"));
        nameEditor.onReturnKey = [this] { applyName(); };
        nameEditor.onFocusLost = [this] { applyName(); };
        addAndMakeVisible (nameEditor);

        addAndMakeVisible (people);

        chatTitle.setText (TRANS ("Chat").toUpperCase(), juce::dontSendNotification);
        chatTitle.setFont (theme::ui (10.0f, theme::Weight::bold).withExtraKerningFactor (0.1f));
        chatTitle.setColour (juce::Label::textColourId, theme::colours::muted);
        addAndMakeVisible (chatTitle);

        chatViewport.setViewedComponent (&chatView, false);
        chatViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (chatViewport);

        input.setTextToShowWhenEmpty (TRANS ("Write a message..."), theme::colours::dim);
        theme::setStyle (sendButton, "accent");
        sendButton.setButtonText (TRANS ("Send"));
        input.onReturnKey = [this] { send(); };
        addAndMakeVisible (input);
        sendButton.onClick = [this] { send(); };
        addAndMakeVisible (sendButton);
    }

    void refreshPeople()
    {
        if (! nameEditor.hasKeyboardFocus (true))
            nameEditor.setText (state.session.getUserName(), false);

        people.repaint();
        resized();
    }

    void refreshChat()
    {
        const bool atBottom = chatViewport.getViewPositionY() + chatViewport.getHeight() >= chatView.getHeight() - 4;
        chatView.update (chatViewport.getWidth() - chatViewport.getScrollBarThickness());

        if (atBottom)
            chatViewport.setViewPosition (0, juce::jmax (0, chatView.getHeight() - chatViewport.getHeight()));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        auto nameRow = r.removeFromTop (28);
        nameLabel.setBounds (nameRow.removeFromLeft (74));
        nameEditor.setBounds (nameRow);
        r.removeFromTop (8);

        const int numPeople = state.session.getPeople().size();
        people.setBounds (r.removeFromTop (juce::jmin (6, numPeople) * PeopleView::rowHeight));
        r.removeFromTop (8);

        chatTitle.setBounds (r.removeFromTop (18));
        auto inputRow = r.removeFromBottom (28);
        sendButton.setBounds (inputRow.removeFromRight (60));
        inputRow.removeFromRight (4);
        input.setBounds (inputRow);
        r.removeFromBottom (4);
        chatViewport.setBounds (r);
        refreshChat();
    }

private:
    //==============================================================================
    struct PeopleView  : public juce::Component
    {
        explicit PeopleView (EditorState& s) : state (s) {}
        static constexpr int rowHeight = 34;

        void paint (juce::Graphics& g) override
        {
            auto* edit = state.getEdit();
            auto r = getLocalBounds();

            for (auto& p : state.session.getPeople())
            {
                auto row = r.removeFromTop (rowHeight).reduced (0, 2);
                g.setColour (rowColour);
                g.fillRoundedRectangle (row.toFloat(), 6.0f);
                row.reduce (6, 0);

                // A coloured initial, like the session badge.
                auto circle = row.removeFromLeft (22).withSizeKeepingCentre (22, 22).toFloat();
                g.setColour (p.colour);
                g.fillEllipse (circle);
                g.setColour (juce::Colours::white);
                g.setFont (theme::ui (10.5f, theme::Weight::bold));
                g.drawText (p.name.substring (0, 1).toUpperCase(), circle, juce::Justification::centred, false);
                row.removeFromLeft (8);

                auto top = row.removeFromTop (row.getHeight() / 2 + 1);
                g.setColour (theme::colours::text);
                g.setFont (theme::ui (12.5f, theme::Weight::bold));
                g.drawText (p.name + (p.isMe ? " (" + TRANS ("you") + ")" : juce::String()) + (p.isHost ? juce::String::fromUTF8 (" \xc2\xb7 ") + TRANS ("host") : juce::String()),
                            top, juce::Justification::bottomLeft, true);

                juce::String what;

                if (p.recording)
                {
                    g.setColour (theme::colours::red);
                    what = juce::String::fromUTF8 ("\xe2\x97\x8f ") + TRANS ("recording");
                }
                else
                {
                    g.setColour (theme::colours::muted);
                    auto pattern = edit != nullptr ? findPattern (*edit, p.viewing) : juce::ValueTree();
                    what = pattern.isValid() ? TRANS ("on") + " " + pattern[ids::name].toString() : juce::String();
                }

                // Playback: sharing it, listening along, or in Focus.
                juce::StringArray tags;

                if (p.sharing)    tags.add (TRANS ("sharing playback"));
                if (p.listening)  tags.add (TRANS ("listening along"));
                if (p.focus)      tags.add (TRANS ("Focus"));

                if (! tags.isEmpty())
                    what += (what.isEmpty() ? juce::String() : juce::String::fromUTF8 (" \xc2\xb7 ")) + tags.joinIntoString (", ");

                g.setFont (theme::ui (10.5f));
                g.drawText (what, row, juce::Justification::topLeft, true);
            }
        }

        EditorState& state;
    };

    //==============================================================================
    struct ChatView  : public juce::Component
    {
        explicit ChatView (EditorState& s) : state (s) {}

        juce::AttributedString lineText (const collab::Session::ChatLine& line) const
        {
            juce::AttributedString a;
            a.setWordWrap (juce::AttributedString::byWord);
            a.append (line.time + "  ", theme::mono (10.5f), theme::colours::dim);

            if (line.system)
            {
                a.append (line.text, theme::ui (11.5f).italicised(), theme::colours::muted);
            }
            else
            {
                a.append (line.name + " ", theme::ui (12.0f, theme::Weight::bold), line.colour);
                a.append (line.text, theme::ui (12.0f), theme::colours::text);
            }

            return a;
        }

        void update (int width)
        {
            layouts.clear();
            int height = 4;

            for (auto& line : state.session.getChat())
            {
                auto* layout = layouts.add (new juce::TextLayout());
                layout->createLayout (lineText (line), (float) juce::jmax (40, width - 8));
                height += (int) std::ceil (layout->getHeight()) + 4;
            }

            setSize (juce::jmax (40, width), juce::jmax (height, 10));
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            float y = 4.0f;

            for (auto* layout : layouts)
            {
                layout->draw (g, { 4.0f, y, (float) getWidth() - 8.0f, layout->getHeight() });
                y += layout->getHeight() + 4.0f;
            }
        }

        EditorState& state;
        juce::OwnedArray<juce::TextLayout> layouts;
    };

    Browser& owner;
    EditorState& state;
    juce::Label nameLabel, chatTitle;
    juce::TextEditor nameEditor, input;
    juce::TextButton sendButton;
    PeopleView people { state };
    ChatView chatView { state };
    juce::Viewport chatViewport;

    void applyName()
    {
        const auto name = nameEditor.getText().trim();

        if (name.isNotEmpty() && name != state.session.getUserName())
        {
            state.session.setUserName (name);

            if (owner.onUserNameChanged)
                owner.onUserNameChanged (state.session.getUserName());
        }
    }

    void send()
    {
        state.session.sendChat (input.getText());
        input.clear();
    }
};

//==============================================================================
Browser::Browser (EditorState& s) : state (s)
{
    patterns = std::make_unique<PatternList> (*this);
    audio = std::make_unique<AudioList> (*this);
    sessionPanel = std::make_unique<SessionPanel> (*this);

    addChildComponent (*patterns);
    addChildComponent (*audio);
    addChildComponent (*sessionPanel);

    const juce::String names[] = { TRANS ("Patterns"), TRANS ("Audio"), TRANS ("Session") };

    for (int i = 0; i < 3; ++i)
    {
        tabButtons[i].setButtonText (names[i]);
        tabButtons[i].setClickingTogglesState (false);
        theme::setStyle (tabButtons[i], "flat");
        tabButtons[i].setColour (juce::TextButton::textColourOffId, theme::colours::muted);
        tabButtons[i].setColour (juce::TextButton::textColourOnId, theme::colours::text);
        tabButtons[i].onClick = [this, i] { showTab (i); };
        addAndMakeVisible (tabButtons[i]);
    }

    collapseButton.setTooltip (TRANS ("Hide the browser (F8 shows it again)"));
    collapseButton.setButtonText ({});
    theme::setIcon (collapseButton, theme::Icon::chevronLeft);
    theme::setStyle (collapseButton, "flat");
    collapseButton.onClick = [this] { if (onCollapse) onCollapse(); };
    addAndMakeVisible (collapseButton);

    showTab (patternsTab);
    startTimerHz (2);
}

Browser::~Browser()
{
    stopTimer();
}

void Browser::showTab (int tab)
{
    current = juce::jlimit (0, 2, tab);
    patterns->setVisible (current == patternsTab);
    audio->setVisible (current == audioTab);
    sessionPanel->setVisible (current == sessionTab);

    if (current == sessionTab)
        seenChatLines = state.session.getChat().size();

    updateTabButtons();
}

void Browser::updateTabButtons()
{
    const int unread = state.session.getChat().size() - seenChatLines;

    for (int i = 0; i < 3; ++i)
        tabButtons[i].setToggleState (i == current, juce::dontSendNotification);

    // Unread chat: the Session tab says how many, in the accent colour.
    const bool showUnread = current != sessionTab && unread > 0;
    tabButtons[sessionTab].setButtonText (showUnread ? TRANS ("Session") + " (" + juce::String (unread) + ")" : TRANS ("Session"));
    tabButtons[sessionTab].setColour (juce::TextButton::textColourOffId, showUnread ? theme::colours::accent : theme::colours::muted);
}

void Browser::refresh()
{
    patterns->refresh();
    audio->refresh();
}

void Browser::refreshPeople()
{
    patterns->refresh();   // the dots of who's on which pattern
    sessionPanel->refreshPeople();
}

void Browser::refreshChat()
{
    if (current == sessionTab)
        seenChatLines = state.session.getChat().size();

    sessionPanel->refreshChat();
    updateTabButtons();
}

void Browser::timerCallback()
{
    // Transfer progress of files that haven't arrived yet.
    if (current == audioTab && audio->anyMissing())
        audio->refresh();
}

void Browser::paint (juce::Graphics& g)
{
    theme::fillPanel (g, getLocalBounds());
    g.setColour (theme::colours::line);
    g.fillRect (1, 38, getWidth() - 2, 1);
}

void Browser::paintOverChildren (juce::Graphics& g)
{
    theme::framePanel (g, getLocalBounds());
}

void Browser::resized()
{
    auto r = getLocalBounds().reduced (1);
    auto tabs = r.removeFromTop (38).reduced (6, 6);
    collapseButton.setBounds (tabs.removeFromRight (24));
    tabs.removeFromRight (4);
    const int w = tabs.getWidth() / 3;

    for (int i = 0; i < 3; ++i)
    {
        tabButtons[i].setBounds (i == 2 ? tabs : tabs.removeFromLeft (w));
        tabButtons[i].getProperties().set ("small", true);
    }

    r.removeFromTop (2);
    r.removeFromBottom (6);

    for (auto* c : std::initializer_list<juce::Component*> { patterns.get(), audio.get(), sessionPanel.get() })
        c->setBounds (r);
}

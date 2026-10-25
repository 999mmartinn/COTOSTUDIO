#include "Browser.h"

using namespace song;

static const juce::Colour rowColour (0xff2f353d), panelColour (0xff2a2f36);

static juce::Colour patternColourOf (const juce::ValueTree& pattern)
{
    auto c = pattern[ids::colour].toString();
    return c.isNotEmpty() ? juce::Colour::fromString (c) : juce::Colour (0xff5fb85f);
}

/** Asks for a line of text (rename dialogs). */
static void askForText (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> onOK)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("text", initial);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
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

        newButton.setTooltip ("Make a new, empty pattern");
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
    }

    void resized() override
    {
        auto r = getLocalBounds();
        newButton.setBounds (r.removeFromBottom (26).reduced (4, 0));
        r.removeFromBottom (4);
        list.setBounds (r);
    }

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
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (3, 2);

        g.setColour (viewed ? colour.withAlpha (0.30f) : rowColour);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (6).toFloat(), 2.0f);
        r.removeFromLeft (6);

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

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.setFont (11.0f);
        g.drawText (juce::String (getPatternChannels (*edit, p).size()) + " ch", r.removeFromRight (34),
                    juce::Justification::centredRight);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (13.0f, viewed ? juce::Font::bold : juce::Font::plain));
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
        m.addItem (1, "Open");
        m.addItem (2, "Rename...");
        m.addItem (3, "Clone");
        m.addItem (4, "Split by channel" + (numChannels >= 2 ? " (" + juce::String (numChannels) + " patterns)" : juce::String()),
                   numChannels >= 2);
        m.addSeparator();
        m.addItem (5, "Delete", getPatterns (*edit).size() > 1);

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

        importButton.setTooltip ("Add audio files (WAV, MP3, FLAC...) to the song");
        importButton.onClick = [this] { if (owner.onImportAudio) owner.onImportAudio(); };
        addAndMakeVisible (importButton);
    }

    void refresh()
    {
        auto* edit = state.getEdit();
        assets = edit != nullptr ? getAssets (*edit) : juce::Array<juce::ValueTree>();
        list.updateContent();
        list.repaint();
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
        importButton.setBounds (r.removeFromBottom (26).reduced (4, 0));
        r.removeFromBottom (4);
        list.setBounds (r);
    }

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
                                               : juce::Colours::grey;
        const bool selected = channel != nullptr && state.placeAudioChannel == getChannelID (*channel);
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (3, 2);

        g.setColour (selected ? colour.withAlpha (0.30f) : rowColour);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (6).toFloat(), 2.0f);
        r.removeFromLeft (6);

        g.setColour (juce::Colours::white);
        g.setFont (13.0f);
        g.drawText (asset[ids::name].toString(), r.removeFromTop (r.getHeight() / 2 + 1).reduced (2, 0),
                    juce::Justification::bottomLeft, true);

        // Second line: length, and whether the file is here yet / how often it's used.
        auto& store = state.session.getAssetStore();
        const int uses = getAssetUseCount (*edit, hash);
        juce::String info = juce::String ((double) asset[ids::seconds], 1) + " s  -  ";

        if (! store.has (hash))
        {
            const double progress = store.getProgress (hash);
            info << (progress >= 0.0 ? "receiving " + juce::String (juce::roundToInt (progress * 100.0)) + "%"
                                     : juce::String ("waiting for the file..."));
        }
        else
        {
            info << (uses == 0 ? juce::String ("not in the playlist") : juce::String (uses) + (uses == 1 ? " clip" : " clips"));
        }

        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (11.0f);
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
    juce::TextButton importButton { "Import audio..." };
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
        m.addItem (1, "Rename...");
        m.addSeparator();
        m.addItem (2, "Delete from project" + (uses > 0 ? " (and its " + juce::String (uses) + (uses == 1 ? " clip)" : " clips)")
                                                        : juce::String()));

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
        nameLabel.setText ("Your name", juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (nameLabel);

        nameEditor.setText (state.session.getUserName(), false);
        nameEditor.setTooltip ("How the others see you");
        nameEditor.onReturnKey = [this] { applyName(); };
        nameEditor.onFocusLost = [this] { applyName(); };
        addAndMakeVisible (nameEditor);

        addAndMakeVisible (people);

        chatTitle.setText ("Chat", juce::dontSendNotification);
        chatTitle.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        addAndMakeVisible (chatTitle);

        chatViewport.setViewedComponent (&chatView, false);
        chatViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (chatViewport);

        input.setTextToShowWhenEmpty ("Message...", juce::Colours::grey);
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
        auto r = getLocalBounds().reduced (4);
        auto nameRow = r.removeFromTop (24);
        nameLabel.setBounds (nameRow.removeFromLeft (70));
        nameEditor.setBounds (nameRow);
        r.removeFromTop (6);

        const int numPeople = state.session.getPeople().size();
        people.setBounds (r.removeFromTop (juce::jmin (6, numPeople) * PeopleView::rowHeight));
        r.removeFromTop (8);

        chatTitle.setBounds (r.removeFromTop (18));
        auto inputRow = r.removeFromBottom (26);
        sendButton.setBounds (inputRow.removeFromRight (50));
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
                g.fillRoundedRectangle (row.toFloat(), 3.0f);
                row.reduce (6, 0);

                g.setColour (p.colour);
                g.fillEllipse (row.removeFromLeft (12).withSizeKeepingCentre (10, 10).toFloat());
                row.removeFromLeft (6);

                auto top = row.removeFromTop (row.getHeight() / 2 + 1);
                g.setColour (juce::Colours::white);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (p.name + (p.isMe ? " (you)" : juce::String()) + (p.isHost ? "  - host" : juce::String()),
                            top, juce::Justification::bottomLeft, true);

                juce::String what;

                if (p.recording)
                {
                    g.setColour (juce::Colour (0xffe05050));
                    what = juce::String::fromUTF8 ("\xe2\x97\x8f recording");
                }
                else
                {
                    g.setColour (juce::Colours::white.withAlpha (0.5f));
                    auto pattern = edit != nullptr ? findPattern (*edit, p.viewing) : juce::ValueTree();
                    what = pattern.isValid() ? "on " + pattern[ids::name].toString() : juce::String();
                }

                g.setFont (11.0f);
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
            a.append (line.time + "  ", juce::FontOptions (11.0f), juce::Colours::grey);

            if (line.system)
            {
                a.append (line.text, juce::FontOptions (12.0f, juce::Font::italic), juce::Colours::lightgrey);
            }
            else
            {
                a.append (line.name + ": ", juce::FontOptions (13.0f, juce::Font::bold), line.colour);
                a.append (line.text, juce::FontOptions (13.0f), juce::Colours::white);
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
    juce::TextButton sendButton { "Send" };
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

    const char* names[] = { "Patterns", "Audio", "Session" };

    for (int i = 0; i < 3; ++i)
    {
        tabButtons[i].setButtonText (names[i]);
        tabButtons[i].setClickingTogglesState (false);
        tabButtons[i].onClick = [this, i] { showTab (i); };
        addAndMakeVisible (tabButtons[i]);
    }

    collapseButton.setTooltip ("Hide the browser (F8 shows it again)");
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
    {
        const bool on = i == current;
        tabButtons[i].setColour (juce::TextButton::buttonColourId, on ? juce::Colour (0xfff0a030).darker (0.2f) : juce::Colour (0xff3a414a));
        tabButtons[i].setColour (juce::TextButton::textColourOffId, on ? juce::Colours::black : juce::Colours::white);
    }

    tabButtons[sessionTab].setButtonText (current != sessionTab && unread > 0 ? "Session (" + juce::String (unread) + ")"
                                                                               : juce::String ("Session"));

    if (current != sessionTab && unread > 0)
        tabButtons[sessionTab].setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4f7fbf));
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
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

void Browser::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto tabs = r.removeFromTop (24);
    collapseButton.setBounds (tabs.removeFromRight (22));
    tabs.removeFromRight (4);
    const int w = tabs.getWidth() / 3;

    for (int i = 0; i < 3; ++i)
        tabButtons[i].setBounds (i == 2 ? tabs : tabs.removeFromLeft (w));

    r.removeFromTop (6);

    for (auto* c : std::initializer_list<juce::Component*> { patterns.get(), audio.get(), sessionPanel.get() })
        c->setBounds (r);
}

#include "SoundBrowser.h"

static const juce::Colour panelColour (0xff2a2f36), selectColour (0xfff0a030),
                          folderColour (0xffd8b45f), audioColour (0xff45c4b0), midiColour (0xffd86f8f);

//==============================================================================
// Little drawn icons (no image files needed).
static void drawFolderIcon (juce::Graphics& g, juce::Rectangle<float> r, bool open)
{
    r = r.withSizeKeepingCentre (14.0f, 11.0f);
    juce::Path p;
    p.startNewSubPath (r.getX(), r.getBottom());
    p.lineTo (r.getX(), r.getY() + 1.0f);
    p.lineTo (r.getX() + 5.0f, r.getY() + 1.0f);
    p.lineTo (r.getX() + 6.5f, r.getY() + 2.5f);
    p.lineTo (r.getRight(), r.getY() + 2.5f);
    p.lineTo (r.getRight(), r.getBottom());
    p.closeSubPath();
    g.setColour (folderColour.withAlpha (open ? 0.55f : 1.0f));
    g.fillPath (p);

    if (open)
    {
        g.setColour (folderColour);
        g.fillRect (r.withTrimmedTop (5.0f).translated (1.5f, 0.0f).withWidth (r.getWidth() - 1.0f));
    }
}

static void drawAudioIcon (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    r = r.withSizeKeepingCentre (14.0f, 12.0f);
    const float heights[] = { 0.35f, 0.8f, 1.0f, 0.55f, 0.75f, 0.3f };
    g.setColour (c);

    for (int i = 0; i < 6; ++i)
    {
        const float h = r.getHeight() * heights[i];
        g.fillRoundedRectangle (r.getX() + (float) i * 2.4f, r.getCentreY() - h / 2.0f, 1.6f, h, 0.8f);
    }
}

static void drawMidiIcon (juce::Graphics& g, juce::Rectangle<float> r)
{
    r = r.withSizeKeepingCentre (14.0f, 12.0f);
    g.setColour (midiColour);
    g.drawRoundedRectangle (r, 1.5f, 1.2f);

    for (int i = 1; i < 4; ++i)
        g.fillRect (r.getX() + (float) i * 3.5f - 0.5f, r.getY(), 1.0f, r.getHeight());

    g.fillRect (r.getX() + 2.0f, r.getY(), 2.0f, r.getHeight() * 0.6f);
    g.fillRect (r.getX() + 9.0f, r.getY(), 2.0f, r.getHeight() * 0.6f);
}

//==============================================================================
class SoundBrowser::Item  : public juce::TreeViewItem
{
public:
    enum class Kind { root, folder, audio, midi, note };

    Item (SoundBrowser& o, Kind k, juce::File f, bool top = false, juce::String noteText = {})
        : owner (o), kind (k), file (std::move (f)), topLevel (top), text (std::move (noteText)) {}

    bool mightContainSubItems() override        { return kind == Kind::root || kind == Kind::folder; }
    bool canBeSelected() const override          { return kind != Kind::note && kind != Kind::root; }
    int getItemHeight() const override           { return topLevel ? 26 : 22; }
    juce::String getUniqueName() const override
    {
        // (Never empty: the tree only remembers which folders are open for items with a name.)
        return kind == Kind::root ? juce::String ("sounds") : kind == Kind::note ? "note:" + text : file.getFullPathName();
    }

    void itemOpennessChanged (bool isNowOpen) override
    {
        if (isNowOpen && kind == Kind::folder && ! loaded)
            loadChildren();
    }

    /** Fills in this folder's subfolders and sounds (only when it is first opened). */
    void loadChildren()
    {
        loaded = true;
        clearSubItems();

        if (! file.isDirectory())
        {
            addSubItem (new Item (owner, Kind::note, {}, false, "Folder not found (unplugged drive?)"));
            return;
        }

        auto children = file.findChildFiles (juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false);
        juce::Array<juce::File> dirs, sounds;

        for (auto& c : children)
        {
            if (c.isDirectory())
                dirs.add (c);
            else if (c.hasFileExtension (fileimport::audioExtensions) || c.hasFileExtension (fileimport::midiExtensions))
                sounds.add (c);
        }

        struct ByName
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                return a.getFileName().compareNatural (b.getFileName());
            }
        } byName;

        dirs.sort (byName);
        sounds.sort (byName);

        for (auto& d : dirs)
            addSubItem (new Item (owner, Kind::folder, d));

        for (auto& f : sounds)
            addSubItem (new Item (owner, f.hasFileExtension (fileimport::midiExtensions) ? Kind::midi : Kind::audio, f));

        if (dirs.isEmpty() && sounds.isEmpty())
            addSubItem (new Item (owner, Kind::note, {}, false, "No sounds or MIDI here"));
    }

    void refreshFolder()
    {
        if (kind != Kind::folder)
            return;

        loadChildren();
        setOpen (true);
    }

    void paintItem (juce::Graphics& g, int width, int height) override
    {
        auto r = juce::Rectangle<int> (0, 0, width, height).reduced (0, 1).toFloat();

        if (isSelected())
        {
            g.setColour (selectColour.withAlpha (0.28f));
            g.fillRoundedRectangle (r, 3.0f);
        }

        if (kind == Kind::note)
        {
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.setFont (juce::FontOptions (12.0f, juce::Font::italic));
            g.drawText (text, r.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
            return;
        }

        auto icon = r.removeFromLeft (20.0f);

        if (kind == Kind::folder)
            drawFolderIcon (g, icon, isOpen());
        else if (kind == Kind::midi)
            drawMidiIcon (g, icon);
        else
            drawAudioIcon (g, icon, owner.previewer.isPlaying() && owner.previewer.getFile() == file
                                        ? juce::Colours::white : audioColour);

        r.removeFromLeft (4.0f);
        const bool missing = topLevel && ! file.isDirectory();
        g.setColour (missing ? juce::Colours::white.withAlpha (0.4f) : juce::Colours::white);
        g.setFont (juce::FontOptions (topLevel ? 13.5f : 12.5f, topLevel ? juce::Font::bold : juce::Font::plain));
        auto name = kind == Kind::folder || kind == Kind::root ? file.getFileName() : file.getFileNameWithoutExtension();

        if (name.isEmpty())
            name = file.getFullPathName();   // a whole drive

        if (showParent)
        {
            // Search results: which folder it's in, after the name.
            const auto nameWidth = juce::jmin ((int) r.getWidth(),
                                               juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 8);
            g.drawText (name, r.removeFromLeft ((float) nameWidth), juce::Justification::centredLeft, true);
            g.setColour (juce::Colours::white.withAlpha (0.4f));
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (file.getParentDirectory().getFileName(), r, juce::Justification::centredLeft, true);
            return;
        }

        g.drawText (name + (missing ? "  (not found)" : juce::String()), r.reduced (0.0f, 0.0f),
                    juce::Justification::centredLeft, true);
    }

    void itemClicked (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            owner.showItemMenu (*this);
            return;
        }

        if (kind == Kind::folder)
            setOpen (! isOpen());
        else if (kind == Kind::audio && juce::Time::getMillisecondCounter() - owner.lastPreviewStart > 250)
            owner.preview (file);   // clicking the same sound again plays it again
    }

    void itemDoubleClicked (const juce::MouseEvent&) override {}   // a single click already opens folders

    void itemSelectionChanged (bool isNowSelected) override
    {
        if (! isNowSelected)
            return;

        if (owner.restoring)
        {
            // Back where you left it: show the sound, but don't play it at start-up.
            if (kind == Kind::audio || kind == Kind::midi)
                owner.showFile (file);

            return;
        }

        if (kind == Kind::audio)
            owner.preview (file);      // also when moving with the arrow keys
        else if (kind == Kind::midi)
            owner.showFile (file);
    }

    juce::var getDragSourceDescription() override
    {
        if (kind == Kind::audio || kind == Kind::midi)
            return fileimport::dragDescriptionFor (file);

        return {};
    }

    juce::String getTooltip() override
    {
        switch (kind)
        {
            case Kind::audio:  return file.getFullPathName() + "\nClick: listen (only you). Drag onto the channel rack or the playlist to use it in the song.";
            case Kind::midi:   return file.getFullPathName() + "\nDrag onto a channel (or the piano roll) to put its notes in the pattern.";
            case Kind::folder: return file.getFullPathName();
            case Kind::root:
            case Kind::note:
            default:           return {};
        }
    }

    SoundBrowser& owner;
    const Kind kind;
    const juce::File file;
    const bool topLevel;
    const juce::String text;
    bool showParent = false, loaded = false;
};

//==============================================================================
/** Looks for sounds by name in all your folders, on its own thread. */
class SoundBrowser::SearchJob  : public juce::Thread
{
public:
    SearchJob (SoundBrowser& o, int searchID, juce::Array<juce::File> roots, juce::String query)
        : juce::Thread ("Sound search"), owner (&o), id (searchID), folders (std::move (roots))
    {
        words.addTokens (query.toLowerCase(), " ", "\"");
        words.removeEmptyStrings();
        startThread (juce::Thread::Priority::low);
    }

    ~SearchJob() override   { stopThread (4000); }

    void run() override
    {
        juce::Array<juce::File> found;
        bool truncated = false;
        constexpr int maxResults = 500;

        for (auto& root : folders)
        {
            if (! root.isDirectory())
                continue;

            for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if (threadShouldExit())
                    return;

                const auto f = entry.getFile();

                if (! f.hasFileExtension (fileimport::audioExtensions) && ! f.hasFileExtension (fileimport::midiExtensions))
                    continue;

                const auto name = f.getFileName().toLowerCase();
                bool all = true;

                for (auto& w : words)
                    all = all && name.contains (w);

                if (all)
                {
                    found.add (f);

                    if (found.size() >= maxResults)
                    {
                        truncated = true;
                        break;
                    }
                }
            }

            if (truncated)
                break;
        }

        juce::MessageManager::callAsync ([safe = owner, id = id, found, truncated]
        {
            if (safe != nullptr)
                safe->showResults (id, found, truncated);
        });
    }

private:
    juce::Component::SafePointer<SoundBrowser> owner;   // made on the message thread
    const int id;
    juce::Array<juce::File> folders;
    juce::StringArray words;
};

//==============================================================================
SoundBrowser::SoundBrowser (SamplePreviewer& p)
    : previewer (p), thumbnail (256, p.getFormats(), thumbCache)
{
    root = std::make_unique<Item> (*this, Item::Kind::root, juce::File());
    tree.setRootItem (root.get());
    tree.setRootItemVisible (false);
    tree.setIndentSize (14);
    tree.setDefaultOpenness (false);
    tree.setColour (juce::TreeView::backgroundColourId, juce::Colours::transparentBlack);
    tree.setColour (juce::TreeView::linesColourId, juce::Colours::transparentBlack);
    tree.setColour (juce::TreeView::selectedItemBackgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (tree);

    searchBox.setTextToShowWhenEmpty ("Search your sounds...", juce::Colours::white.withAlpha (0.35f));
    searchBox.setFont (juce::FontOptions (13.0f));
    searchBox.setTooltip ("Finds sounds and MIDI by name in all your folders (several words: all of them)");
    searchBox.onTextChange = [this] { searchCountdown = 6; };   // wait until you stop typing (~0.2 s)
    searchBox.onEscapeKey = [this] { searchBox.clear(); searchCountdown = 1; };
    addAndMakeVisible (searchBox);

    addButton.setTooltip ("Add a folder of yours (your Beats folder, a sample pack...). Only on this PC");
    addButton.onClick = [this] { chooseFolder(); };
    addAndMakeVisible (addButton);

    bigAddButton.setTooltip (addButton.getTooltip());
    bigAddButton.onClick = [this] { chooseFolder(); };
    addChildComponent (bigAddButton);

    collapseButton.setTooltip ("Hide your sounds (Alt+F8 shows them again)");
    collapseButton.onClick = [this] { if (onCollapse) onCollapse(); };
    addAndMakeVisible (collapseButton);

    playButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb6"));
    playButton.setTooltip ("Play / stop this sound (only you hear it)");
    playButton.onClick = [this]
    {
        if (previewer.isPlaying())
            previewer.stop();
        else if (fileimport::isAudioFile (shownFile))
            preview (shownFile);
    };
    addChildComponent (playButton);

    volume.setRange (0.0, 1.0);
    volume.setValue (previewer.getGain(), juce::dontSendNotification);
    volume.setTooltip ("How loud sounds play while you browse (only you)");
    volume.onValueChange = [this] { previewer.setGain ((float) volume.getValue()); };
    addAndMakeVisible (volume);

    previewer.addChangeListener (this);
    thumbnail.addChangeListener (this);
    rebuildTree();
    startTimerHz (30);
}

SoundBrowser::~SoundBrowser()
{
    stopTimer();
    search.reset();
    previewer.removeChangeListener (this);
    thumbnail.removeChangeListener (this);
    tree.setRootItem (nullptr);
}

//==============================================================================
void SoundBrowser::setFolders (const juce::StringArray& paths)
{
    folders.clear();

    for (auto& p : paths)
        if (juce::File::isAbsolutePath (p))
            folders.addIfNotAlreadyThere (juce::File (p));

    rebuildTree();
}

juce::StringArray SoundBrowser::getFolders() const
{
    juce::StringArray paths;

    for (auto& f : folders)
        paths.add (f.getFullPathName());

    return paths;
}

void SoundBrowser::addFolder (const juce::File& folder)
{
    if (! folder.isDirectory() || folders.contains (folder))
        return;

    folders.add (folder);
    foldersChanged();

    // Open it right away.
    for (int i = 0; i < root->getNumSubItems(); ++i)
        if (auto* item = dynamic_cast<Item*> (root->getSubItem (i)))
            if (item->file == folder)
            {
                item->setOpen (true);
                tree.scrollToKeepItemVisible (item);
            }
}

void SoundBrowser::removeFolder (const juce::File& folder)
{
    folders.removeAllInstancesOf (folder);
    foldersChanged();
}

void SoundBrowser::foldersChanged()
{
    auto openness = tree.getOpennessState (true);
    rebuildTree();

    if (openness != nullptr)
        tree.restoreOpennessState (*openness, true);

    if (onFoldersChanged)
        onFoldersChanged (getFolders());

    if (searchBox.getText().isNotEmpty())
        startSearch();
}

void SoundBrowser::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a folder of sounds (your Beats folder, a pack...)",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
    {
        if (fc.getResult().isDirectory())
            addFolder (fc.getResult());
    });
}

void SoundBrowser::rebuildTree()
{
    showingResults = false;
    root->clearSubItems();

    for (auto& f : folders)
        root->addSubItem (new Item (*this, Item::Kind::folder, f, true));

    root->setOpen (true);
    tree.setVisible (! folders.isEmpty());
    bigAddButton.setVisible (folders.isEmpty());
    searchBox.setEnabled (! folders.isEmpty());
    repaint();
}

//==============================================================================
void SoundBrowser::startSearch()
{
    search.reset();   // stops the one still running
    const auto query = searchBox.getText().trim();
    ++searchId;

    if (query.isEmpty())
    {
        if (showingResults)
        {
            rebuildTree();

            if (opennessBeforeSearch != nullptr)
                tree.restoreOpennessState (*opennessBeforeSearch, true);

            opennessBeforeSearch.reset();
        }

        return;
    }

    if (! showingResults)
        opennessBeforeSearch = tree.getOpennessState (true);

    showingResults = true;
    root->clearSubItems();
    root->addSubItem (new Item (*this, Item::Kind::note, {}, false, "Searching..."));
    search = std::make_unique<SearchJob> (*this, searchId, folders, query);
}

void SoundBrowser::showResults (int id, const juce::Array<juce::File>& found, bool truncated)
{
    if (id != searchId || ! showingResults)
        return;   // an older search

    root->clearSubItems();

    for (auto& f : found)
    {
        auto* item = new Item (*this, f.hasFileExtension (fileimport::midiExtensions) ? Item::Kind::midi : Item::Kind::audio, f);
        item->showParent = true;
        root->addSubItem (item);
    }

    if (found.isEmpty())
        root->addSubItem (new Item (*this, Item::Kind::note, {}, false, "No sounds match"));
    else if (truncated)
        root->addSubItem (new Item (*this, Item::Kind::note, {}, false, "First 500 shown - type more to narrow it down"));
}

juce::String SoundBrowser::getState() const
{
    juce::XmlElement state ("SOUNDS");

    // While searching, the folders as they were before the search.
    std::unique_ptr<juce::XmlElement> openness;

    if (showingResults)
    {
        if (opennessBeforeSearch != nullptr)
            openness = std::make_unique<juce::XmlElement> (*opennessBeforeSearch);
    }
    else
    {
        openness = tree.getOpennessState (true);
    }

    if (openness != nullptr)
        state.addChildElement (openness.release());

    if (shownFile != juce::File())
        state.setAttribute ("shown", shownFile.getFullPathName());

    return state.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
}

void SoundBrowser::restoreState (const juce::String& text)
{
    pendingState = juce::parseXML (text);

    if (pendingState != nullptr && ! pendingState->hasTagName ("SOUNDS"))
        pendingState.reset();

    applyPendingState();
}

void SoundBrowser::applyPendingState()
{
    if (pendingState == nullptr || tree.getHeight() <= 0 || folders.isEmpty())
        return;

    const auto state = std::move (pendingState);
    const juce::ScopedValueSetter<bool> svs (restoring, true);

    if (auto* openness = state->getFirstChildElement())
        tree.restoreOpennessState (*openness, true);

    const juce::File shown (state->getStringAttribute ("shown"));

    if (juce::File::isAbsolutePath (shown.getFullPathName()) && shown.existsAsFile())
        showFile (shown);
}

void SoundBrowser::setPreviewVolume (float v)
{
    previewer.setGain (v);
    volume.setValue (previewer.getGain(), juce::dontSendNotification);
}

//==============================================================================
void SoundBrowser::preview (const juce::File& file)
{
    lastPreviewStart = juce::Time::getMillisecondCounter();
    showFile (file);

    if (! previewer.play (file))
        shownInfo = "Can't play this file";

    tree.repaint();
}

void SoundBrowser::showFile (const juce::File& file)
{
    shownFile = file;
    shownInfo = {};

    if (fileimport::isMidiFile (file))
    {
        auto midi = fileimport::readMidiFile (file);
        shownInfo = midi.ok() ? juce::String (midi.notes.size()) + " notes, "
                                   + juce::String (juce::jmax (1, (int) std::ceil (midi.lengthBeats / 4.0 - 1.0e-9))) + " bars"
                              : midi.error;
        thumbnail.clear();
    }
    else if (std::unique_ptr<juce::AudioFormatReader> r { previewer.getFormats().createReaderFor (file) })
    {
        const double seconds = r->sampleRate > 0.0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
        shownInfo = (seconds < 10.0 ? juce::String (seconds, 2) : juce::String (seconds, 1)) + " s  -  "
                    + juce::String (r->sampleRate / 1000.0, 1) + " kHz  -  " + (r->numChannels > 1 ? "stereo" : "mono");
        thumbnail.setSource (new juce::FileInputSource (file));
    }

    playButton.setVisible (fileimport::isAudioFile (file));
    repaint (previewArea);
}

void SoundBrowser::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint (previewArea);
    tree.repaint();
}

void SoundBrowser::timerCallback()
{
    if (searchCountdown > 0 && --searchCountdown == 0)
        startSearch();

    // The preview's playhead: repaint only while something plays (and once when it stops).
    const bool playing = previewer.isPlaying();

    if (playing || wasPlaying)
    {
        repaint (waveArea);

        if (playing != wasPlaying)
        {
            playButton.setButtonText (juce::String::fromUTF8 (playing ? "\xe2\x96\xa0" : "\xe2\x96\xb6"));
            tree.repaint();
        }
    }

    wasPlaying = playing;
}

//==============================================================================
void SoundBrowser::showItemMenu (Item& item)
{
    juce::PopupMenu m;
    const auto file = item.file;
    const bool top = item.topLevel;

    if (item.kind == Item::Kind::audio)
    {
        m.addItem (1, "Listen");
        m.addItem (2, "New channel in the rack with it");
        m.addItem (3, "Add to the playlist (at the start marker)");
    }
    else if (item.kind == Item::Kind::midi)
    {
        m.addItem (4, "Notes into the selected channel (this pattern)");
        m.addItem (5, "New Synth channel with these notes");
    }
    else if (item.kind == Item::Kind::folder)
    {
        m.addItem (6, "Refresh");
    }
    else
    {
        return;
    }

    m.addItem (7, "Show in Explorer", file.exists());

    if (top)
    {
        m.addSeparator();
        m.addItem (8, "Remove from this list (the folder stays on your PC)");
    }

    m.showMenuAsync (juce::PopupMenu::Options(), [this, file, safe = juce::Component::SafePointer<SoundBrowser> (this)] (int r)
    {
        if (safe == nullptr || r == 0)
            return;

        switch (r)
        {
            case 1: preview (file); break;
            case 2: if (onAddToRack) onAddToRack (file); break;
            case 3: if (onAddToPlaylist) onAddToPlaylist (file); break;
            case 4: if (onMidiToSelectedChannel) onMidiToSelectedChannel (file); break;
            case 5: if (onMidiToNewChannel) onMidiToNewChannel (file); break;
            case 6:
                if (auto* folderItem = findFolderItem (root.get(), file))
                    folderItem->refreshFolder();
                break;
            case 7: file.revealToUser(); break;
            case 8: removeFolder (file); break;
            default: break;
        }
    });
}

SoundBrowser::Item* SoundBrowser::findFolderItem (juce::TreeViewItem* parent, const juce::File& folder)
{
    for (int i = 0; parent != nullptr && i < parent->getNumSubItems(); ++i)
    {
        if (auto* item = dynamic_cast<Item*> (parent->getSubItem (i)))
        {
            if (item->kind == Item::Kind::folder && item->file == folder)
                return item;

            if (item->kind == Item::Kind::folder && folder.isAChildOf (item->file))
                return findFolderItem (item, folder);
        }
    }

    return nullptr;
}

void SoundBrowser::mouseDown (const juce::MouseEvent& e)
{
    // Clicking the waveform plays the sound from the start.
    if (waveArea.contains (e.getPosition()) && fileimport::isAudioFile (shownFile))
        preview (shownFile);
}

//==============================================================================
void SoundBrowser::paint (juce::Graphics& g)
{
    g.setColour (panelColour);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

    // Title
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("Sounds", getLocalBounds().reduced (8, 4).removeFromTop (24), juce::Justification::centredLeft, false);

    if (folders.isEmpty())
    {
        auto r = tree.getBounds().withTrimmedTop (60).reduced (10, 0);
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText ("Add the folders where you keep your drum kits, loops and MIDI. They stay on this PC: "
                          "a sound only joins the song (and is sent to the others) when you drag it onto the "
                          "channel rack, the playlist or the piano roll.",
                          r.removeFromTop (110), juce::Justification::centredTop, 7);
    }

    // The sound at the bottom: name, waveform with playhead, details.
    if (previewArea.isEmpty())
        return;

    g.setColour (juce::Colour (0xff23272d));
    g.fillRoundedRectangle (previewArea.toFloat(), 4.0f);

    if (shownFile == juce::File())
    {
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText ("Click a sound to hear it", previewArea.reduced (8), juce::Justification::centred, 2);
        return;
    }

    auto r = previewArea.reduced (8, 5);
    auto top = r.removeFromTop (20);
    top.removeFromLeft (playButton.isVisible() ? 28 : 0);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (shownFile.getFileName(), top, juce::Justification::centredLeft, true);

    auto infoRow = r.removeFromBottom (14);
    infoRow.removeFromRight (86);   // the volume slider
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (shownInfo, infoRow, juce::Justification::centredLeft, true);

    // Waveform
    if (! waveArea.isEmpty() && fileimport::isAudioFile (shownFile))
    {
        g.setColour (juce::Colour (0xff1b1e23));
        g.fillRoundedRectangle (waveArea.toFloat(), 3.0f);

        if (thumbnail.getTotalLength() > 0.0)
        {
            g.setColour (audioColour);
            thumbnail.drawChannels (g, waveArea.reduced (2), 0.0, thumbnail.getTotalLength(), 1.0f);
        }

        if (previewer.isPlaying() && previewer.getFile() == shownFile && previewer.getLengthSeconds() > 0.0)
        {
            const float x = (float) waveArea.getX()
                            + (float) (previewer.getPositionSeconds() / previewer.getLengthSeconds()) * (float) waveArea.getWidth();
            g.setColour (juce::Colours::white);
            g.fillRect (x, (float) waveArea.getY(), 1.5f, (float) waveArea.getHeight());
        }
    }
    else if (fileimport::isMidiFile (shownFile))
    {
        g.setColour (midiColour.withAlpha (0.7f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText ("MIDI: drag it onto a channel or the piano roll", waveArea, juce::Justification::centred, 2);
    }
}

void SoundBrowser::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto header = r.removeFromTop (24);
    collapseButton.setBounds (header.removeFromRight (22));
    header.removeFromRight (4);
    addButton.setBounds (header.removeFromRight (74));

    r.removeFromTop (6);
    searchBox.setBounds (r.removeFromTop (26));
    r.removeFromTop (6);

    previewArea = r.removeFromBottom (92);
    r.removeFromBottom (6);
    tree.setBounds (r);
    bigAddButton.setBounds (r.removeFromTop (34).reduced (10, 0));

    applyPendingState();   // the first time the column has a size

    auto p = previewArea.reduced (8, 5);
    playButton.setBounds (p.removeFromTop (20).removeFromLeft (24));
    auto bottom = p.removeFromBottom (14);
    volume.setBounds (bottom.removeFromRight (80).withSizeKeepingCentre (80, 18));
    p.removeFromBottom (4);
    p.removeFromTop (4);
    waveArea = p;
}

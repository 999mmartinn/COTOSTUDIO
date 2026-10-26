#include "SoundBrowser.h"

static const juce::Colour selectColour = theme::colours::accent, folderColour = theme::colours::amber,
                          audioColour = theme::colours::teal, midiColour = theme::colours::pink;

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
    // Outlined in amber; filled when open (like the design).
    g.setColour (folderColour.withAlpha (open ? 0.35f : 0.12f));
    g.fillPath (p);
    g.setColour (folderColour);
    g.strokePath (p, juce::PathStrokeType (1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
            g.setColour (theme::colours::accentSoft);
            g.fillRoundedRectangle (r, 5.0f);
        }

        if (kind == Kind::note)
        {
            g.setColour (theme::colours::dim);
            g.setFont (theme::ui (11.5f).italicised());
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
                                        ? theme::colours::text : audioColour);

        r.removeFromLeft (4.0f);
        const bool missing = topLevel && ! file.isDirectory();
        g.setColour (missing ? theme::colours::dim : isSelected() ? theme::colours::text : theme::colours::text.withAlpha (0.92f));
        g.setFont (theme::ui (topLevel ? 12.5f : 12.0f, topLevel ? theme::Weight::bold : isSelected() ? theme::Weight::semibold : theme::Weight::regular));
        auto name = kind == Kind::folder || kind == Kind::root ? file.getFileName() : file.getFileNameWithoutExtension();

        if (name.isEmpty())
            name = file.getFullPathName();   // a whole drive

        if (showParent)
        {
            // Search results: which folder it's in, after the name.
            const auto nameWidth = juce::jmin ((int) r.getWidth(),
                                               juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 8);
            g.drawText (name, r.removeFromLeft ((float) nameWidth), juce::Justification::centredLeft, true);
            g.setColour (theme::colours::muted);
            g.setFont (theme::ui (10.5f));
            g.drawText (file.getParentDirectory().getFileName(), r, juce::Justification::centredLeft, true);
            return;
        }

        g.drawText (name + (missing ? "  (" + TRANS ("not found") + ")" : juce::String()), r.reduced (0.0f, 0.0f),
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

    searchBox.setTextToShowWhenEmpty (TRANS ("Search your sounds..."), theme::colours::dim);
    searchBox.setFont (theme::ui (12.0f));
    searchBox.setIndents (28, 7);   // room for the magnifier
    searchBox.setTooltip (TRANS ("Finds sounds and MIDI by name in all your folders (several words: all of them)"));
    searchBox.onTextChange = [this] { searchCountdown = 6; };   // wait until you stop typing (~0.2 s)
    searchBox.onEscapeKey = [this] { searchBox.clear(); searchCountdown = 1; };
    addAndMakeVisible (searchBox);

    addButton.setTooltip (TRANS ("Add a folder of yours (your Beats folder, a sample pack...). Only on this PC"));
    addButton.setButtonText (TRANS ("Folder"));
    theme::setIcon (addButton, theme::Icon::plus);
    addButton.getProperties().set ("small", true);
    bigAddButton.setButtonText (TRANS ("Add your Beats folder..."));
    theme::setIcon (bigAddButton, theme::Icon::folder);
    theme::setStyle (bigAddButton, "accent");
    addButton.onClick = [this] { chooseFolder(); };
    addAndMakeVisible (addButton);

    bigAddButton.setTooltip (addButton.getTooltip());
    bigAddButton.onClick = [this] { chooseFolder(); };
    addChildComponent (bigAddButton);

    collapseButton.setTooltip (TRANS ("Hide your sounds (Alt+F8 shows them again)"));
    collapseButton.setButtonText ({});
    theme::setIcon (collapseButton, theme::Icon::chevronLeft);
    collapseButton.onClick = [this] { if (onCollapse) onCollapse(); };
    addAndMakeVisible (collapseButton);

    theme::setIcon (playButton, theme::Icon::play);
    theme::setStyle (playButton, "accent");
    playButton.setTooltip (TRANS ("Play / stop this sound (only you hear it)"));
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
    volume.setTooltip (TRANS ("How loud sounds play while you browse (only you)"));
    volume.setColour (juce::Slider::trackColourId, theme::colours::accent);
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
    chooser = std::make_unique<juce::FileChooser> (TRANS ("Choose a folder of sounds (your Beats folder, a pack...)"),
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
            theme::setIcon (playButton, playing ? theme::Icon::stop : theme::Icon::play);
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
        m.addItem (1, TRANS ("Listen"));
        m.addItem (2, TRANS ("New channel in the rack with it"));
        m.addItem (3, TRANS ("Add to the playlist (at the start marker)"));
    }
    else if (item.kind == Item::Kind::midi)
    {
        m.addItem (4, TRANS ("Notes into the selected channel (this pattern)"));
        m.addItem (5, TRANS ("New Synth channel with these notes"));
    }
    else if (item.kind == Item::Kind::folder)
    {
        m.addItem (6, TRANS ("Refresh"));
    }
    else
    {
        return;
    }

    m.addItem (7, TRANS ("Show in Explorer"), file.exists());

    if (top)
    {
        m.addSeparator();
        m.addItem (8, TRANS ("Remove from this list (the folder stays on your PC)"));
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
    using namespace theme::colours;
    theme::fillPanel (g, getLocalBounds());

    // Title: the note icon and SOUNDS.
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (12, 0);
    theme::drawIcon (g, theme::Icon::sounds, header.removeFromLeft (16).toFloat().withSizeKeepingCentre (16.0f, 16.0f), accent);
    header.removeFromLeft (8);
    g.setColour (text);
    g.setFont (theme::ui (13.0f, theme::Weight::bold).withExtraKerningFactor (0.04f));
    g.drawText (TRANS ("Sounds").toUpperCase(), header, juce::Justification::centredLeft, false);

    // The magnifier in the search box, and a line under it.
    const auto sb = searchBox.getBounds();
    g.setColour (line);
    g.fillRect (sb.getX(), sb.getBottom() + 7, sb.getWidth(), 1);

    if (folders.isEmpty())
    {
        auto r = tree.getBounds().withTrimmedTop (52).reduced (12, 0);
        g.setColour (muted);
        g.setFont (theme::ui (12.0f));
        g.drawFittedText (TRANS ("Add the folders where you keep your drum kits, loops and MIDI. They stay on this PC: "
                                 "a sound only joins the song (and is sent to the others) when you drag it onto the "
                                 "channel rack, the playlist or the piano roll."),
                          r.removeFromTop (120), juce::Justification::centredTop, 8);
    }

    // A reminder that this column is yours only (dashed box with a lock).
    if (! noteArea.isEmpty())
    {
        juce::Path outline, dashed;
        outline.addRoundedRectangle (noteArea.toFloat().reduced (0.5f), 7.0f);
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (well);
        g.fillRoundedRectangle (noteArea.toFloat(), 7.0f);
        g.setColour (gridBar);
        g.fillPath (dashed);

        auto n = noteArea.reduced (9, 6);
        auto lock = n.removeFromLeft (14).removeFromTop (14).toFloat();
        g.setColour (muted);
        g.drawRoundedRectangle (lock.withTrimmedTop (5.5f).reduced (1.5f, 0.5f), 1.5f, 1.2f);
        juce::Path shackle;
        shackle.addCentredArc (lock.getCentreX(), lock.getY() + 5.5f, 2.6f, 3.0f, 0.0f,
                               -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        g.strokePath (shackle, juce::PathStrokeType (1.2f));
        n.removeFromLeft (7);
        g.setFont (theme::ui (10.5f));
        g.drawFittedText (TRANS ("Only you see these folders. A sound is shared when you drop it into the rack, the playlist or the piano roll."),
                          n, juce::Justification::centredLeft, 3, 1.0f);
    }

    // The sound at the bottom: name, waveform with playhead, details.
    if (previewArea.isEmpty())
        return;

    g.setColour (lcd);
    g.fillRoundedRectangle (previewArea.toFloat(), 9.0f);
    g.setColour (line);
    g.drawRoundedRectangle (previewArea.toFloat().reduced (0.5f), 9.0f, 1.0f);

    if (shownFile == juce::File())
    {
        g.setColour (lcdMuted);
        g.setFont (theme::ui (12.0f));
        g.drawFittedText (TRANS ("Click a sound to hear it"), previewArea.reduced (8), juce::Justification::centred, 2);
        return;
    }

    auto r = previewArea.reduced (10, 8);
    auto top = r.removeFromTop (28);
    top.removeFromLeft (playButton.isVisible() ? 34 : 0);
    g.setColour (text);
    g.setFont (theme::ui (12.0f, theme::Weight::bold));
    g.drawText (shownFile.getFileName(), top.removeFromTop (15), juce::Justification::centredLeft, true);
    g.setColour (lcdMuted);
    g.setFont (theme::mono (10.0f));
    g.drawText (shownInfo, top, juce::Justification::centredLeft, true);

    auto infoRow = r.removeFromBottom (16);
    g.setColour (lcdMuted);
    g.setFont (theme::ui (10.0f));
    g.drawText (TRANS ("Only you"), infoRow.removeFromRight (56), juce::Justification::centredRight, false);
    theme::drawIcon (g, theme::Icon::wave, infoRow.removeFromLeft (14).toFloat().withSizeKeepingCentre (12.0f, 12.0f), lcdMuted);

    // Waveform
    if (! waveArea.isEmpty() && fileimport::isAudioFile (shownFile))
    {
        if (thumbnail.getTotalLength() > 0.0)
        {
            g.setColour (lcdText);
            thumbnail.drawChannels (g, waveArea.reduced (0, 2), 0.0, thumbnail.getTotalLength(), 1.0f);
        }

        if (previewer.isPlaying() && previewer.getFile() == shownFile && previewer.getLengthSeconds() > 0.0)
        {
            const float x = (float) waveArea.getX()
                            + (float) (previewer.getPositionSeconds() / previewer.getLengthSeconds()) * (float) waveArea.getWidth();
            g.setColour (juce::Colours::white);
            g.fillRoundedRectangle (x, (float) waveArea.getY(), 2.0f, (float) waveArea.getHeight(), 1.0f);
        }
    }
    else if (fileimport::isMidiFile (shownFile))
    {
        g.setColour (midiColour);
        g.setFont (theme::ui (11.5f));
        g.drawFittedText (TRANS ("MIDI: drag it onto a channel or the piano roll"), waveArea, juce::Justification::centred, 2);
    }
}

void SoundBrowser::paintOverChildren (juce::Graphics& g)
{
    // The magnifier inside the search box.
    const auto sb = searchBox.getBounds();
    theme::drawIcon (g, theme::Icon::search, juce::Rectangle<float> ((float) sb.getX() + 9.0f, (float) sb.getCentreY() - 6.5f, 13.0f, 13.0f),
                     theme::colours::muted);
    theme::framePanel (g, getLocalBounds());
}

void SoundBrowser::resized()
{
    auto r = getLocalBounds().reduced (1);
    auto header = r.removeFromTop (headerHeight - 1).reduced (8, 7);
    collapseButton.setBounds (header.removeFromRight (26));
    header.removeFromRight (6);
    addButton.setBounds (header.removeFromRight (86));

    r.reduce (8, 0);
    searchBox.setBounds (r.removeFromTop (30));
    r.removeFromTop (14);

    previewArea = r.removeFromBottom (112);
    r.removeFromBottom (8);
    noteArea = folders.isEmpty() || r.getHeight() < 260 ? juce::Rectangle<int>() : r.removeFromBottom (48);

    if (! noteArea.isEmpty())
        r.removeFromBottom (8);

    tree.setBounds (r);
    bigAddButton.setBounds (r.removeFromTop (34).reduced (10, 0));

    applyPendingState();   // the first time the column has a size

    auto p = previewArea.reduced (10, 8);
    playButton.setBounds (p.removeFromTop (28).removeFromLeft (26).withSizeKeepingCentre (26, 26));
    auto bottom = p.removeFromBottom (16);
    bottom.removeFromLeft (20);
    bottom.removeFromRight (60);
    volume.setBounds (bottom);
    p.removeFromBottom (6);
    p.removeFromTop (6);
    waveArea = p;
}

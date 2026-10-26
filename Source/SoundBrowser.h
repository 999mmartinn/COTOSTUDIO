#pragma once

#include "SamplePreviewer.h"
#include "FileImport.h"
#include "Theme.h"

/**
    "Sounds": your own folders of drum kits, loops, one-shots and MIDI, in a
    column on the far left (like FL's browser).

    Everything here is yours only: the list of folders is a setting on this PC,
    and browsing or previewing never touches the song.
      - "+ Folder" adds a folder (your Beats folder, a sample pack...).
      - Click a sound to hear it (only you hear it). Up/down keys: the next one.
      - Drag a sound onto the channel rack (a new channel, or a new sound for a
        sampler channel) or onto the playlist (an audio clip); drag a MIDI file
        onto a channel or the piano roll. Only then does it become part of the
        song and get sent to the others.
      - The search box finds sounds by name in all your folders.
*/
class SoundBrowser  : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit SoundBrowser (SamplePreviewer&);
    ~SoundBrowser() override;

    /** Your folders (restored from the settings at start-up). */
    void setFolders (const juce::StringArray& paths);
    juce::StringArray getFolders() const;
    void addFolder (const juce::File&);
    void removeFolder (const juce::File&);

    /** How loud sounds play while browsing (0..1). */
    void setPreviewVolume (float);

    /** Plays a sound and shows it at the bottom. */
    void preview (const juce::File&);

    /** Where you were - open folders, scroll position, the sound you picked - as text to
        keep in the settings, and putting it back (next start). */
    juce::String getState() const;
    void restoreState (const juce::String&);

    /** Called when your list of folders changes (to save it). */
    std::function<void (const juce::StringArray&)> onFoldersChanged;
    std::function<void()> onCollapse;

    // Right-click actions: the same as dropping the file there.
    std::function<void (const juce::File&)> onAddToRack, onAddToPlaylist, onMidiToSelectedChannel, onMidiToNewChannel;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int defaultWidth = 280, headerHeight = 42;

private:
    class Item;
    class SearchJob;

    SamplePreviewer& previewer;
    juce::Array<juce::File> folders;
    std::unique_ptr<Item> root;
    juce::TreeView tree;
    juce::TextEditor searchBox;
    juce::TextButton addButton { "+ Folder" }, collapseButton { "<" }, bigAddButton { "Add your Beats folder..." },
                     playButton;
    juce::Slider volume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::unique_ptr<juce::FileChooser> chooser;

    // The sound at the bottom (selected / playing)
    juce::File shownFile;
    juce::String shownInfo;
    juce::AudioThumbnailCache thumbCache { 4 };
    juce::AudioThumbnail thumbnail;
    juce::Rectangle<int> previewArea, waveArea, noteArea;
    juce::uint32 lastPreviewStart = 0;
    bool wasPlaying = false;

    // Search
    std::unique_ptr<SearchJob> search;
    int searchId = 0, searchCountdown = 0;
    std::unique_ptr<juce::XmlElement> opennessBeforeSearch;
    bool showingResults = false;

    // Putting back where you were (once the column has its size, so the scroll position fits)
    std::unique_ptr<juce::XmlElement> pendingState;
    bool restoring = false;
    void applyPendingState();

    void rebuildTree();
    void startSearch();
    void showResults (int id, const juce::Array<juce::File>& found, bool truncated);
    void chooseFolder();
    void showFile (const juce::File&);
    void foldersChanged();
    void showItemMenu (Item&);
    Item* findFolderItem (juce::TreeViewItem* parent, const juce::File& folder);

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    friend class Item;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundBrowser)
};

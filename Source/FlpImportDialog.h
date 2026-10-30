#pragma once

#include "FlpImport.h"

/**
    File > Import an FL Studio project: shows what the .flp holds (the mixer inserts in
    use, with their effects and where they go; a beat's channels, patterns and playlist,
    and which of its sounds were found) and lets you untick what you don't need before
    the song is built. "Find missing sounds..." looks for the beat's sounds in a folder.
*/
class FlpImportDialog  : public juce::Component
{
public:
    using Callback = std::function<void (const flimport::Project&, const flimport::Options&)>;

    FlpImportDialog (const flimport::Project&, const juce::File& flpFile, Callback onImport);
    ~FlpImportDialog() override;

    /** Opens the dialog in its own window. */
    static void show (const flimport::Project&, const juce::File& flpFile, Callback onImport);

    /** A window with a long text (the import report). */
    static void showText (const juce::String& title, const juce::String& text);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row;
    flimport::Project project;
    const juce::File flpFile;
    Callback onImport;

    juce::Label intro;
    juce::OwnedArray<Row> rows;
    juce::Component list;
    juce::Viewport viewport;
    juce::ToggleButton laneNames, skipOff, beatToggle;
    juce::Label soundsLabel;
    juce::TextButton allButton, noneButton, importButton, cancelButton, findSoundsButton;
    std::unique_ptr<juce::FileChooser> chooser;

    void close();
    void updateImportButton();
    void updateSounds();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlpImportDialog)
};

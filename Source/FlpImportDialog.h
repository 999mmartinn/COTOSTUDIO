#pragma once

#include "FlpImport.h"

/**
    File > Templates > Import an FL Studio template: shows what the .flp holds (the
    mixer inserts in use, with their effects and where they go) and lets you untick
    the ones you don't need before the song is built.
*/
class FlpImportDialog  : public juce::Component
{
public:
    FlpImportDialog (const flimport::Project&, const juce::String& fileName,
                     std::function<void (const flimport::Options&)> onImport);
    ~FlpImportDialog() override;

    /** Opens the dialog in its own window. */
    static void show (const flimport::Project&, const juce::String& fileName,
                      std::function<void (const flimport::Options&)> onImport);

    /** A window with a long text (the import report). */
    static void showText (const juce::String& title, const juce::String& text);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row;
    const flimport::Project project;
    std::function<void (const flimport::Options&)> onImport;

    juce::Label intro;
    juce::OwnedArray<Row> rows;
    juce::Component list;
    juce::Viewport viewport;
    juce::ToggleButton laneNames, skipOff;
    juce::TextButton allButton, noneButton, importButton, cancelButton;

    void close();
    void updateImportButton();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlpImportDialog)
};

#pragma once

#include "EditorState.h"

/**
    FL-style browser on the left, with three tabs:
      - Patterns: every pattern; click to open it, drag it onto the playlist,
        right-click to rename / clone / split by channel / delete.
        Coloured dots show who else is looking at a pattern.
      - Audio: every audio file in the song (recorded takes and imported files)
        with its length and whether it has arrived here yet; click to select it
        for placing, drag it onto the playlist, right-click to rename or delete it
        from the project for good; "Delete unused takes" clears out every take that
        has no clip in the playlist.
      - Session: your name, who is connected (and what they're doing), and chat.
*/
class Browser  : public juce::Component,
                 private juce::Timer
{
public:
    explicit Browser (EditorState&);
    ~Browser() override;

    void refresh();          // the song changed
    void refreshPeople();    // someone joined/left/moved
    void refreshChat();      // a chat line arrived

    enum Tab { patternsTab = 0, audioTab, sessionTab };
    void showTab (int tab);

    std::function<void (const juce::String& patternSid)> onPatternChosen;
    std::function<void()> onCollapse;
    std::function<void()> onImportAudio;
    std::function<void()> onDeleteUnusedTakes;
    std::function<void (const juce::String& name)> onUserNameChanged;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    static constexpr int defaultWidth = 240;

private:
    class PatternList;
    class AudioList;
    class SessionPanel;

    EditorState& state;
    juce::TextButton tabButtons[3], collapseButton { "<" };
    std::unique_ptr<PatternList> patterns;
    std::unique_ptr<AudioList> audio;
    std::unique_ptr<SessionPanel> sessionPanel;
    int current = patternsTab;
    int seenChatLines = 0;

    void updateTabButtons();
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Browser)
};

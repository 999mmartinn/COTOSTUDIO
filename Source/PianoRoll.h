#pragma once

#include "EditorState.h"

/**
    Piano roll for the selected channel in the viewed pattern, with FL's three main tools:
      - Pencil (P): click empty space to add a note (as long as the last note you touched)
        and drag to move it; drag a note to move it, its right edge to change its length;
        right-click or right-drag deletes. Ctrl+drag selects, Shift+drag copies.
      - Select (E): drag a box to select notes, then drag one of them to move them all;
        Ctrl+click adds or removes one note.
      - Slice (C): click a note, or drag down across several, to cut them at that point.
    Keys: Ctrl+A / Ctrl+D select all / none, Ctrl+C / X / V copy / cut / paste,
    Ctrl+B duplicate after itself, Ctrl+L legato, Ctrl+Q quantize, Delete,
    Up / Down a semitone (Ctrl or Shift: an octave), Left / Right one grid step.
    Ctrl+wheel zooms in time, around the mouse. The instrument whose notes you edit is
    picked in the title row (its menu, or < >, or Alt+Up / Alt+Down). Notes snap to the chosen grid (Alt = free).
    Notes are heard while placed or dragged only when the song is stopped (the keyboard
    on the left always plays). The Scale button keeps notes on a key's scale.
    The ruler on top sets where Play starts (jumps there while playing).
*/
class PianoRoll  : public juce::Component,
                   private juce::Timer
{
public:
    explicit PianoRoll (EditorState&);
    ~PianoRoll() override;

    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

    /** Gives the keyboard to the notes (so the shortcuts work straight away). */
    void focusNotes();

    static constexpr int lowestPitch = 24, highestPitch = 96, rowHeight = 14, keyboardWidth = 48;

    /** Own window (e.g. a second monitor) and full screen (set up by the main window). */
    std::function<void()> onDetachToggle, onFullScreenToggle;
    void setDetached (bool);
    void setFullScreenState (bool isFullScreen);
    /** The scale helper or the snap changed (to remember them). */
    std::function<void()> onScaleChanged;

private:
    class Grid;
    class Ruler;
    class Keys;
    class ChannelPicker;

    struct NotesViewport  : public juce::Viewport
    {
        std::function<void()> onMoved;
        void visibleAreaChanged (const juce::Rectangle<int>&) override    { if (onMoved) onMoved(); }
    };

    EditorState& state;
    std::unique_ptr<Grid> grid;
    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<Keys> keys;
    std::unique_ptr<ChannelPicker> picker;   // which instrument's notes you edit (menu, < >)
    juce::TextButton prevChannelButton, nextChannelButton;
    /** The previous (-1) or next (+1) instrument in the rack. */
    void stepChannel (int direction);
    void showChannelMenu();
    juce::Component keysHolder;     // clips the keyboard, which scrolls with the notes
    NotesViewport viewport;
    juce::Label title;
    juce::TextButton pencilButton, selectButton, sliceButton, snapButton, scaleButton, detachButton, fullScreenButton;
    bool detached = false;
    double zoom = 1.0;              // 1 = the whole pattern fits the width
    double sizedForLength = -1.0;
    void showScaleMenu();
    void updateScaleButton();
    void showSnapMenu();
    void updateSnapButton();
    void updateTools();
    void updateHint();
    void updateGridSize();
    void zoomAround (float factor, float gridX);
    void viewMoved();
    juce::String heading;
    juce::Colour headingColour { theme::colours::muted };
    static constexpr int titleHeight = 32;
    int lastPlayhead = -2;
    bool scrolledOnce = false;

    void timerCallback() override;
};

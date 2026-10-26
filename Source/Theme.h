#pragma once

#include <JuceHeader.h>

/**
    COTOSTUDIO's look ("Nocturno"): a dark purple-grey with a light lilac accent.

    Everything is flat (fills and thin borders, no blur, shadows or animations) so it
    stays light on the graphics card: only things that move (playhead, meters, the
    counter) repaint while playing.
*/
namespace theme
{
    //==============================================================================
    namespace colours
    {
        inline const juce::Colour bg          { 0xff14111c };   // window
        inline const juce::Colour panel       { 0xff1f1b2a };   // panels
        inline const juce::Colour well        { 0xff17141f };   // buttons, fields
        inline const juce::Colour wellAlt     { 0xff1a1724 };   // inside panels (dock, grids)
        inline const juce::Colour raised      { 0xff27223a };   // hover
        inline const juce::Colour line        { 0xff332d46 };   // borders
        inline const juce::Colour text        { 0xffeeeaf8 };
        inline const juce::Colour muted       { 0xffa79fc6 };
        inline const juce::Colour dim         { 0xff6e6690 };
        inline const juce::Colour accent      { 0xffb39bff };   // light lilac
        inline const juce::Colour accentSoft  { 0xff2e2747 };   // selection background
        inline const juce::Colour accentText  { 0xff17131f };   // text on the accent
        inline const juce::Colour lcd         { 0xff0f0d16 };
        inline const juce::Colour lcdText     { 0xffd4c7ff };
        inline const juce::Colour lcdMuted    { 0xff8e82b8 };
        inline const juce::Colour lane1       { 0xff1d1a28 };
        inline const juce::Colour lane2       { 0xff1a1724 };
        inline const juce::Colour grid        { 0xff29243a };
        inline const juce::Colour gridBar     { 0xff40385a };
        inline const juce::Colour stepOff     { 0xff2b2639 };
        inline const juce::Colour stepOffAlt  { 0xff352e48 };
        inline const juce::Colour red         { 0xfff2637a };
        inline const juce::Colour redSoft     { 0xff3d2232 };
        inline const juce::Colour green       { 0xff3dba7e };
        inline const juce::Colour teal        { 0xff38c9b6 };
        inline const juce::Colour amber       { 0xffe8b530 };
        inline const juce::Colour sky         { 0xff3fa2db };
        inline const juce::Colour pink        { 0xffe0559a };
        inline const juce::Colour orange      { 0xfff08a3c };
        inline const juce::Colour mint        { 0xff38b37c };
        inline const juce::Colour violet      { 0xff9b7bf0 };
        inline const juce::Colour lilac       { 0xffc9b6ff };
    }

    /** Colours for channels, patterns, people (in order). */
    juce::Colour channelColour (int index);

    //==============================================================================
    // Fonts: Inter for the interface, JetBrains Mono for numbers (both built in).
    enum class Weight { regular, medium, semibold, bold };
    juce::Font ui (float height, Weight = Weight::regular);
    juce::Font mono (float height, bool bold = false);

    //==============================================================================
    // Icons, drawn (no image files), designed on a 16 x 16 grid.
    enum class Icon
    {
        none = 0, play, stop, record, metronome, undo, redo, gear, share, focus, folder, plus, close,
        chevronDown, chevronLeft, chevronRight, detach, attach, fullScreen, note, wave, search, file,
        people, automation, mixer, pianoRoll, check, sounds, menu, link
    };

    void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float> area, juce::Colour, float strokeWidth = 1.6f);

    /** Puts an icon on a TextButton (drawn by the look; the text, if any, follows it). */
    void setIcon (juce::Button&, Icon);
    /** Button styles (drawn by the look): "accent", "play", "record", "flat", "toggle" (accent when on). */
    void setStyle (juce::Button&, const char* style);

    //==============================================================================
    // Panels are rounded cards (like the design): fillPanel in paint(), framePanel in
    // paintOverChildren() so the corners and border stay clean over scrolling content.
    constexpr float panelRadius = 10.0f;
    void fillPanel (juce::Graphics&, juce::Rectangle<int> area, juce::Colour fill = colours::panel);
    void framePanel (juce::Graphics&, juce::Rectangle<int> area);
    /** A panel's title row text: bold title, then a muted note. Returns what's left of the row. */
    juce::Rectangle<int> drawPanelTitle (juce::Graphics&, juce::Rectangle<int> row, const juce::String& title, const juce::String& note = {});
    /** Small caps heading (PATTERNS, CHAT...). */
    void drawHeading (juce::Graphics&, juce::Rectangle<int> area, const juce::String& text, juce::Justification = juce::Justification::centredLeft);

    //==============================================================================
    /** The look of every standard control: buttons, menus, lists, sliders, fields... */
    class Look  : public juce::LookAndFeel_V4
    {
    public:
        Look();

        juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

        // Buttons
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
        void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;

        // Combo boxes and menus
        void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
        void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
        void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;
        void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
        juce::Font getPopupMenuFont() override;
        void getIdealPopupMenuItemSize (const juce::String&, bool isSeparator, int standardHeight, int& w, int& h) override;
        int getPopupMenuBorderSize() override                              { return 4; }

        // Text
        void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
        void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
        juce::Font getLabelFont (juce::Label&) override;
        juce::Font getAlertWindowMessageFont() override;
        juce::Font getAlertWindowTitleFont() override;
        juce::Font getAlertWindowFont() override;
        void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
        int getAlertWindowButtonHeight() override                          { return 30; }
        void drawTooltip (juce::Graphics&, const juce::String& text, int w, int h) override;
        juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;

        // Sliders and knobs
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;
        juce::Label* createSliderTextBox (juce::Slider&) override;
        int getSliderThumbRadius (juce::Slider&) override;
        juce::Button* createSliderButton (juce::Slider&, bool isIncrement) override;

        // Scroll bars, tabs, trees, dividers, progress
        void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                            int thumbSize, bool over, bool down) override;
        int getDefaultScrollbarWidth() override                            { return 10; }
        int getTabButtonBestWidth (juce::TabBarButton&, int depth) override;
        void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool over, bool down) override;
        void drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&) override;
        void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int w, int h) override;
        void drawTreeviewPlusMinusBox (juce::Graphics&, const juce::Rectangle<float>& area, juce::Colour, bool isOpen, bool over) override;
        void drawStretchableLayoutResizerBar (juce::Graphics&, int w, int h, bool vertical, bool over, bool dragging) override;
        void drawProgressBar (juce::Graphics&, juce::ProgressBar&, int w, int h, double progress, const juce::String& text) override;
        void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image&) override;

    private:
        juce::Typeface::Ptr inter[4], monoRegular, monoBold;
    };

    /** The one shared look (set as the default for the whole app). */
    Look& look();

    /** The COTOSTUDIO logo badge (from the artwork). */
    juce::Image logoBadge();
    juce::Image logoFull();
}

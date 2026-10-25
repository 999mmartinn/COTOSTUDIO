#include "Theme.h"
#include "BinaryData.h"

namespace theme
{

using namespace colours;

juce::Colour channelColour (int index)
{
    static const juce::Colour list[] = { orange, sky, amber, pink, mint, violet, teal, lilac };
    return list[(size_t) ((index % 8) + 8) % 8];
}

//==============================================================================
static juce::Typeface::Ptr loadFont (const char* originalName)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        if (juce::String (BinaryData::originalFilenames[i]) == originalName)
        {
            int size = 0;

            if (auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
                return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
        }

    return {};
}

static juce::Image loadImage (const char* originalName)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        if (juce::String (BinaryData::originalFilenames[i]) == originalName)
        {
            int size = 0;

            if (auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
                return juce::ImageCache::getFromMemory (data, size);
        }

    return {};
}

juce::Image logoBadge()   { return loadImage ("logo-badge.jpg"); }
juce::Image logoFull()    { return loadImage ("logo-full.jpg"); }

struct Fonts
{
    juce::Typeface::Ptr inter[4], monoRegular, monoBold;

    Fonts()
    {
        inter[0] = loadFont ("Inter-Regular.ttf");
        inter[1] = loadFont ("Inter-Medium.ttf");
        inter[2] = loadFont ("Inter-SemiBold.ttf");
        inter[3] = loadFont ("Inter-Bold.ttf");
        monoRegular = loadFont ("JetBrainsMono-Regular.ttf");
        monoBold = loadFont ("JetBrainsMono-Bold.ttf");
    }
};

static Fonts& fonts()
{
    static Fonts f;
    return f;
}

juce::Font ui (float height, Weight weight)
{
    if (auto t = fonts().inter[(int) weight])
        return juce::Font (juce::FontOptions (t).withHeight (height));

    return juce::Font (juce::FontOptions (height, weight >= Weight::semibold ? juce::Font::bold : juce::Font::plain));
}

juce::Font mono (float height, bool bold)
{
    if (auto t = bold ? fonts().monoBold : fonts().monoRegular)
        return juce::Font (juce::FontOptions (t).withHeight (height));

    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
struct IconShape
{
    const char* stroke;   // SVG path data, drawn as lines
    const char* fill;     // SVG path data, filled
};

static IconShape shapeOf (Icon icon)
{
    switch (icon)
    {
        case Icon::play:          return { nullptr, "M4.5 2.5 L13.5 8 L4.5 13.5 Z" };
        case Icon::stop:          return { nullptr, "M3.5 3.5 H12.5 V12.5 H3.5 Z" };
        case Icon::record:        return { nullptr, "M8 3 A5 5 0 1 1 7.99 3 Z" };
        case Icon::metronome:     return { "M5.8 1.8 H10.2 L13.4 14.2 H2.6 Z M8 10 L11.6 3.6", nullptr };
        case Icon::undo:          return { "M5 3.5 L2 6.5 L5 9.5 M2 6.5 H10 A3.5 3.5 0 0 1 10 13.5 H7", nullptr };
        case Icon::redo:          return { "M11 3.5 L14 6.5 L11 9.5 M14 6.5 H6 A3.5 3.5 0 0 0 6 13.5 H9", nullptr };
        case Icon::gear:          return { "M8 5.4 A2.6 2.6 0 1 1 7.99 5.4 Z M8 1.5 V3.5 M8 12.5 V14.5 M1.5 8 H3.5 M12.5 8 H14.5 M3.4 3.4 L4.8 4.8 "
                                           "M11.2 11.2 L12.6 12.6 M3.4 12.6 L4.8 11.2 M11.2 4.8 L12.6 3.4", nullptr };
        case Icon::share:         return { "M4.8 4.8 A4.5 4.5 0 0 0 4.8 11.2 M11.2 4.8 A4.5 4.5 0 0 1 11.2 11.2 "
                                           "M2.6 2.6 A7.6 7.6 0 0 0 2.6 13.4 M13.4 2.6 A7.6 7.6 0 0 1 13.4 13.4",
                                           "M8 6.2 A1.8 1.8 0 1 1 7.99 6.2 Z" };
        case Icon::focus:         return { "M8 2.8 A5.2 5.2 0 1 1 7.99 2.8 Z", "M8 6.4 A1.6 1.6 0 1 1 7.99 6.4 Z" };
        case Icon::folder:        return { "M1.5 12.5 V3.5 H6 L7.5 5 H14.5 V12.5 Z", nullptr };
        case Icon::plus:          return { "M8 3 V13 M3 8 H13", nullptr };
        case Icon::close:         return { "M4 4 L12 12 M12 4 L4 12", nullptr };
        case Icon::chevronDown:   return { "M4 6 L8 10 L12 6", nullptr };
        case Icon::chevronLeft:   return { "M10 4 L6 8 L10 12", nullptr };
        case Icon::chevronRight:  return { "M6 4 L10 8 L6 12", nullptr };
        case Icon::detach:        return { "M9 2.5 H13.5 V7 M13.5 2.5 L8 8 M11.5 9.5 V13.5 H2.5 V4.5 H6.5", nullptr };
        case Icon::attach:        return { "M13.5 2.5 L8 8 M8 4.5 V8 H11.5 M11.5 9.5 V13.5 H2.5 V4.5 H6.5", nullptr };
        case Icon::fullScreen:    return { "M2.5 6 V2.5 H6 M10 2.5 H13.5 V6 M13.5 10 V13.5 H10 M6 13.5 H2.5 V10", nullptr };
        case Icon::note:
        case Icon::sounds:        return { "M6 12.5 V3.5 L13 2 V11", "M4.3 10.5 A2 2 0 1 1 4.29 10.5 Z M11.3 9 A2 2 0 1 1 11.29 9 Z" };
        case Icon::wave:          return { "M2 7.5 V8.5 M4.5 5 V11 M7 2.5 V13.5 M9.5 5.5 V10.5 M12 4 V12 M14.5 7 V9", nullptr };
        case Icon::search:        return { "M7 2.8 A4.2 4.2 0 1 1 6.99 2.8 Z M10.2 10.2 L13.5 13.5", nullptr };
        case Icon::file:          return { "M4 1.5 H9.5 L12.5 4.5 V14.5 H4 Z M9.5 1.5 V4.5 H12.5", nullptr };
        case Icon::people:        return { "M6 3 A2.5 2.5 0 1 1 5.99 3 Z M1.5 13.5 A4.5 4.5 0 0 1 10.5 13.5 M11 3.2 A2.2 2.2 0 1 1 10.99 3.2 Z "
                                           "M12 9.2 A4 4 0 0 1 14.5 13.5", nullptr };
        case Icon::automation:    return { "M1.5 12 L5 8 L8 9.5 L11 4 L14.5 3", nullptr };
        case Icon::mixer:         return { "M4 2 V14 M8 2 V14 M12 2 V14", "M2.5 9 H5.5 V11 H2.5 Z M6.5 4 H9.5 V6 H6.5 Z M10.5 7 H13.5 V9 H10.5 Z" };
        case Icon::pianoRoll:     return { "M2 2.5 H14 V13.5 H2 Z M2 6.5 H14 M2 10 H14", "M4 3.6 H8 V5.4 H4 Z M8 7.6 H13 V9 H8 Z M5 11.1 H9 V12.4 H5 Z" };
        case Icon::check:         return { "M3 8.5 L6.5 12 L13 4.5", nullptr };
        case Icon::menu:          return { nullptr, "M3 6.6 A1.4 1.4 0 1 1 2.99 6.6 Z M8 6.6 A1.4 1.4 0 1 1 7.99 6.6 Z M13 6.6 A1.4 1.4 0 1 1 12.99 6.6 Z" };
        case Icon::link:          return { "M6.5 9.5 L9.5 6.5 M7 4.5 L8.5 3 A2.8 2.8 0 0 1 13 7.5 L11.5 9 M9 11.5 L7.5 13 A2.8 2.8 0 0 1 3 8.5 L4.5 7", nullptr };
        case Icon::none:
        default:                  return { nullptr, nullptr };
    }
}

void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour, float strokeWidth)
{
    static std::map<int, std::pair<juce::Path, juce::Path>> cache;   // parsed once
    auto it = cache.find ((int) icon);

    if (it == cache.end())
    {
        const auto shape = shapeOf (icon);
        juce::Path s, f;

        if (shape.stroke != nullptr)  s = juce::Drawable::parseSVGPath (shape.stroke);
        if (shape.fill != nullptr)    f = juce::Drawable::parseSVGPath (shape.fill);

        it = cache.emplace ((int) icon, std::make_pair (s, f)).first;
    }

    const float size = juce::jmin (area.getWidth(), area.getHeight());
    const auto box = area.withSizeKeepingCentre (size, size);
    const auto transform = juce::AffineTransform::scale (size / 16.0f).translated (box.getX(), box.getY());

    g.setColour (colour);

    if (! it->second.first.isEmpty())
        g.strokePath (it->second.first, juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      transform);   // (the width is in the 16-unit grid, so it scales with the icon)

    if (! it->second.second.isEmpty())
        g.fillPath (it->second.second, transform);
}

void setIcon (juce::Button& b, Icon icon)        { b.getProperties().set ("icon", (int) icon); b.repaint(); }
void setStyle (juce::Button& b, const char* s)   { b.getProperties().set ("style", juce::String (s)); b.repaint(); }

//==============================================================================
Look::Look()
{
    auto& f = fonts();

    for (int i = 0; i < 4; ++i)
        inter[i] = f.inter[i];

    monoRegular = f.monoRegular;
    monoBold = f.monoBold;

    setColourScheme ({ bg, panel, well, line, text, accent, accentText, panel, text });

    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::DocumentWindow::backgroundColourId, bg);
    setColour (juce::TextButton::buttonColourId, well);
    setColour (juce::TextButton::buttonOnColourId, accentSoft);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, text);
    setColour (juce::ComboBox::backgroundColourId, well);
    setColour (juce::ComboBox::outlineColourId, line);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, muted);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff221d30));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentSoft);
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, muted);
    setColour (juce::TextEditor::backgroundColourId, well);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, line);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.35f));
    setColour (juce::TextEditor::highlightedTextColourId, text);
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::Label::textColourId, text);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ListBox::textColourId, text);
    setColour (juce::TreeView::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::TreeView::linesColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, stepOffAlt);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, stepOffAlt);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, stepOffAlt);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxBackgroundColourId, well);
    setColour (juce::Slider::textBoxOutlineColourId, line);
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, line);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff262138));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, line);
    setColour (juce::TabbedComponent::backgroundColourId, wellAlt);
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TabbedButtonBar::frontOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, dim);
    setColour (juce::ProgressBar::backgroundColourId, stepOff);
    setColour (juce::ProgressBar::foregroundColourId, accent);
    setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, accentSoft);
    setColour (juce::DirectoryContentsDisplayComponent::textColourId, text);
    setColour (juce::FileBrowserComponent::currentPathBoxBackgroundColourId, well);
    setColour (juce::FileBrowserComponent::currentPathBoxTextColourId, text);
    setColour (juce::FileBrowserComponent::filenameBoxBackgroundColourId, well);
    setColour (juce::FileBrowserComponent::filenameBoxTextColourId, text);
}

juce::Typeface::Ptr Look::getTypefaceForFont (const juce::Font& f)
{
    const auto name = f.getTypefaceName();

    if (name == juce::Font::getDefaultMonospacedFontName() && monoRegular != nullptr)
        return f.isBold() ? monoBold : monoRegular;

    if (name == juce::Font::getDefaultSansSerifFontName() && inter[0] != nullptr)
        return f.isBold() ? inter[2] : inter[0];   // "bold" reads best as SemiBold at these sizes

    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

//==============================================================================
static float cornerFor (juce::Rectangle<float> r)   { return juce::jmin (7.0f, r.getHeight() * 0.3f); }

void Look::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& colour, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const auto style = b.getProperties()["style"].toString();
    const bool on = b.getToggleState();
    const float corner = cornerFor (r);
    juce::Colour fill = colour, border = line;

    // Colours set by the code (lights, channel names...) win over the style.
    const bool custom = b.isColourSpecified (on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);

    if (style == "flat")
    {
        if (! (over || down || on))
            return;

        fill = on ? accentSoft : raised;
        border = juce::Colours::transparentBlack;
    }
    else if (style == "accent" || (style == "toggle" && on))
    {
        fill = accent;
        border = accent;
    }
    else if (style == "play")
    {
        fill = green;
        border = green;
    }
    else if (style == "record")
    {
        fill = on ? red : well;
        border = on ? red : line;
    }
    else if (! custom)
    {
        fill = on ? accentSoft : well;
        border = on ? accent : line;
    }
    else
    {
        border = fill.brighter (0.15f);
    }

    if (! b.isEnabled())
        fill = fill.withMultipliedAlpha (0.5f);
    else if (down)
        fill = fill.darker (0.12f);
    else if (over)
        fill = fill.brighter (0.07f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, corner);

    if (! border.isTransparent())
    {
        g.setColour (border);
        g.drawRoundedRectangle (r, corner, 1.0f);
    }
}

juce::Font Look::getTextButtonFont (juce::TextButton&, int h)
{
    return ui (juce::jlimit (11.0f, 13.5f, (float) h * 0.5f), Weight::semibold);
}

void Look::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto style = b.getProperties()["style"].toString();
    const auto icon = (Icon) (int) b.getProperties()["icon"];
    const bool on = b.getToggleState();
    auto textColour = b.findColour (on ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);

    if (style == "accent" || (style == "toggle" && on))
        textColour = accentText;
    else if (style == "play")
        textColour = juce::Colours::white;

    if (! b.isEnabled())
        textColour = textColour.withMultipliedAlpha (0.45f);

    auto r = b.getLocalBounds().toFloat().reduced (6.0f, 2.0f);
    const auto text = b.getButtonText();
    const auto font = getTextButtonFont (b, b.getHeight());
    const float iconSize = juce::jmin (15.0f, r.getHeight() - 2.0f);

    if (icon != Icon::none)
    {
        auto iconColour = textColour;

        if (style == "record" && ! on)
            iconColour = red;

        if (text.isEmpty())
        {
            drawIcon (g, icon, r.withSizeKeepingCentre (iconSize, iconSize), iconColour);
            return;
        }

        // Icon and text centred together.
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
        const float total = juce::jmin (r.getWidth(), iconSize + 6.0f + textWidth);
        auto group = r.withSizeKeepingCentre (total, r.getHeight());
        drawIcon (g, icon, group.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize), iconColour);
        group.removeFromLeft (6.0f);
        g.setColour (textColour);
        g.setFont (font);
        g.drawText (text, group, juce::Justification::centredLeft, true);
        return;
    }

    g.setColour (textColour);
    g.setFont (font);
    g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 2, 0.8f);
}

void Look::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat();
    auto box = r.removeFromLeft (r.getHeight()).reduced (4.0f);
    g.setColour (b.getToggleState() ? accent : (over ? raised : well));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (b.getToggleState() ? accent : line);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);

    if (b.getToggleState())
        drawIcon (g, Icon::check, box.reduced (2.0f), accentText, 2.0f);

    g.setColour (b.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
    g.setFont (ui (13.0f));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (4.0f).toNearestInt(), juce::Justification::centredLeft, 2);
}

//==============================================================================
void Look::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    const float corner = cornerFor (r);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (box.isMouseOver (true) ? 0.07f : 0.0f));
    g.fillRoundedRectangle (r, corner);
    g.setColour (box.hasKeyboardFocus (true) ? accent : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, corner, 1.0f);
    drawIcon (g, Icon::chevronDown, juce::Rectangle<float> ((float) w - 22.0f, 0.0f, 16.0f, (float) h).withSizeKeepingCentre (12.0f, 12.0f),
              box.findColour (juce::ComboBox::arrowColourId), 1.8f);
}

juce::Font Look::getComboBoxFont (juce::ComboBox& box)
{
    return ui (juce::jlimit (11.5f, 13.5f, (float) box.getHeight() * 0.48f), Weight::medium);
}

void Look::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 26, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void Look::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (line);
    g.drawRect (0, 0, w, h, 1);
}

juce::Font Look::getPopupMenuFont()    { return ui (13.5f); }

void Look::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                              bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& textToDraw,
                              const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColourToUse)
{
    if (isSeparator)
    {
        g.setColour (line);
        g.fillRect (area.reduced (8, 0).withSizeKeepingCentre (area.getWidth() - 16, 1));
        return;
    }

    auto r = area.reduced (4, 1);

    if (isHighlighted && isActive)
    {
        g.setColour (accentSoft);
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
    }

    auto colour = textColourToUse != nullptr ? *textColourToUse : text;

    if (! isActive)
        colour = dim;

    r.removeFromLeft (6);
    auto tickArea = r.removeFromLeft (18).toFloat();

    if (isTicked)
        drawIcon (g, Icon::check, tickArea.withSizeKeepingCentre (13.0f, 13.0f), accent, 2.0f);
    else if (icon != nullptr)
        icon->drawWithin (g, tickArea.reduced (2.0f), juce::RectanglePlacement::centred, 1.0f);

    if (hasSubMenu)
        drawIcon (g, Icon::chevronRight, r.removeFromRight (16).toFloat().withSizeKeepingCentre (11.0f, 11.0f), muted, 1.8f);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setFont (mono (11.5f));
        g.setColour (muted);
        g.drawText (shortcutKeyText, r.removeFromRight (110), juce::Justification::centredRight, true);
    }

    g.setFont (getPopupMenuFont());
    g.setColour (colour);
    g.drawFittedText (textToDraw, r.withTrimmedRight (4), juce::Justification::centredLeft, 1);
}

void Look::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setFont (ui (11.0f, Weight::bold));
    g.setColour (muted);
    g.drawText (sectionName.toUpperCase(), area.reduced (12, 0), juce::Justification::bottomLeft, true);
}

//==============================================================================
void Look::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (r, juce::jmin (6.0f, r.getHeight() * 0.3f));
}

void Look::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    if (! e.isEnabled())
        return;

    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (e.hasKeyboardFocus (true) && ! e.isReadOnly() ? e.findColour (juce::TextEditor::focusedOutlineColourId)
                                                               : e.findColour (juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (r, juce::jmin (6.0f, r.getHeight() * 0.3f), 1.0f);
}

juce::Font Look::getLabelFont (juce::Label& l)
{
    const auto f = l.getFont();

    // Labels that kept the default font get the interface font.
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return ui (f.getHeight(), f.isBold() ? Weight::semibold : Weight::regular);

    return f;
}

juce::Font Look::getAlertWindowMessageFont()   { return ui (14.0f); }
juce::Font Look::getAlertWindowTitleFont()     { return ui (17.0f, Weight::bold); }
juce::Font Look::getAlertWindowFont()          { return ui (13.5f); }

void Look::drawAlertBox (juce::Graphics& g, juce::AlertWindow& w, const juce::Rectangle<int>& textArea, juce::TextLayout& layout)
{
    auto r = w.getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (r.removeFromTop (3.0f).reduced (14.0f, 0.0f), 1.5f);
    layout.draw (g, textArea.toFloat().withTrimmedTop (4.0f));
}

void Look::drawTooltip (juce::Graphics& g, const juce::String& t, int w, int h)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (t, ui (13.0f), text);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 360.0f);
    layout.draw (g, r.reduced (10.0f, 6.0f));
}

juce::Rectangle<int> Look::getTooltipBounds (const juce::String& t, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    juce::AttributedString s;
    s.append (t, ui (13.0f), text);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 360.0f);
    const auto w = (int) (layout.getWidth() + 22.0f), h = (int) (layout.getHeight() + 14.0f);

    return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 18,
                                 pos.y > parent.getCentreY() ? pos.y - (h + 8) : pos.y + 8, w, h)
             .constrainedWithin (parent);
}

//==============================================================================
void Look::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const float size = juce::jmin (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (size, size);
    const auto c = r.getCentre();
    const float radius = size * 0.5f - 1.5f, thickness = juce::jmax (2.0f, size * 0.12f);
    const float angle = start + pos * (end - start);

    juce::Path back, value;
    back.addCentredArc (c.x, c.y, radius, radius, 0.0f, start, end, true);
    g.setColour (s.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (back, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Pan-style knobs (bipolar) fill from the middle.
    const bool bipolar = (bool) s.getProperties()["bipolar"];
    const float from = bipolar ? (start + end) * 0.5f : start;

    if (std::abs (angle - from) > 0.01f)
    {
        value.addCentredArc (c.x, c.y, radius, radius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId).withMultipliedAlpha (s.isEnabled() ? 1.0f : 0.4f));
        g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::Path pointer;
    pointer.startNewSubPath (c.getPointOnCircumference (radius * 0.2f, angle));
    pointer.lineTo (c.getPointOnCircumference (radius - thickness * 0.6f, angle));
    g.setColour (text);
    g.strokePath (pointer, juce::PathStrokeType (juce::jmax (1.5f, thickness * 0.75f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void Look::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                             juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style == juce::Slider::LinearVertical)
    {
        // A mixer fader: a thin groove and a wide cap.
        const float cx = (float) x + (float) w * 0.5f;
        g.setColour (stepOffAlt);
        g.fillRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) h, 2.0f);

        const auto cap = juce::Rectangle<float> (juce::jmin (26.0f, (float) w - 2.0f), 28.0f).withCentre ({ cx, pos });
        g.setColour (s.isMouseOverOrDragging() ? juce::Colour (0xffe6e0f8) : juce::Colour (0xffd8d0f0));
        g.fillRoundedRectangle (cap, 4.0f);
        g.setColour (gridBar);
        g.drawRoundedRectangle (cap.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (bg.withAlpha (0.6f));
        g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 8.0f, 2.0f));
        return;
    }

    if (style == juce::Slider::LinearHorizontal)
    {
        const float cy = (float) y + (float) h * 0.5f;
        g.setColour (stepOffAlt);
        g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) w, 4.0f, 2.0f);
        g.setColour (s.findColour (juce::Slider::trackColourId));
        g.fillRoundedRectangle ((float) x, cy - 2.0f, juce::jmax (0.0f, pos - (float) x), 4.0f, 2.0f);
        g.setColour (text);
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ pos, cy }));
        return;
    }

    juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, minPos, maxPos, style, s);
}

juce::Label* Look::createSliderTextBox (juce::Slider& s)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (mono (13.0f, true));
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::textColourId, s.findColour (juce::Slider::textBoxTextColourId));
    return l;
}

juce::Button* Look::createSliderButton (juce::Slider&, bool isIncrement)
{
    auto* b = new juce::TextButton (isIncrement ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92"));
    setStyle (*b, "flat");
    return b;
}

//==============================================================================
void Look::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                          int thumbSize, bool over, bool down)
{
    if (thumbSize <= 0)
        return;

    auto thumb = vertical ? juce::Rectangle<float> ((float) x + 2.5f, (float) thumbStart, (float) w - 5.0f, (float) thumbSize)
                          : juce::Rectangle<float> ((float) thumbStart, (float) y + 2.5f, (float) thumbSize, (float) h - 5.0f);
    g.setColour (down ? muted : (over ? gridBar.brighter (0.3f) : gridBar));
    g.fillRoundedRectangle (thumb, juce::jmin (thumb.getWidth(), thumb.getHeight()) * 0.5f);
}

int Look::getTabButtonBestWidth (juce::TabBarButton& b, int depth)
{
    return (int) juce::GlyphArrangement::getStringWidth (ui (12.5f, Weight::semibold), b.getButtonText()) + depth;
}

void Look::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
{
    auto r = b.getActiveArea().toFloat();
    const bool front = b.isFrontTab();

    if (front)
    {
        g.setColour (wellAlt);
        g.fillRoundedRectangle (r.withTrimmedBottom (-6.0f), 7.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (r.removeFromTop (2.0f).reduced (6.0f, 0.0f), 1.0f);
    }
    else if (over)
    {
        g.setColour (raised.withAlpha (0.6f));
        g.fillRoundedRectangle (r.reduced (0.0f, 2.0f), 6.0f);
    }

    g.setColour (front ? text : muted);
    g.setFont (ui (12.5f, front ? Weight::bold : Weight::semibold));
    g.drawText (b.getButtonText().trim(), b.getTextArea(), juce::Justification::centred, true);
}

void Look::drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&) {}

void Look::drawTabAreaBehindFrontButton (juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h)
{
    g.setColour (line);
    g.fillRect (0, h - 1, w, 1);
    juce::ignoreUnused (bar);
}

void Look::drawTreeviewPlusMinusBox (juce::Graphics& g, const juce::Rectangle<float>& area, juce::Colour, bool isOpen, bool over)
{
    drawIcon (g, isOpen ? Icon::chevronDown : Icon::chevronRight, area.withSizeKeepingCentre (10.0f, 10.0f), over ? text : muted, 2.0f);
}

void Look::drawStretchableLayoutResizerBar (juce::Graphics& g, int w, int h, bool vertical, bool over, bool dragging)
{
    if (! (over || dragging))
        return;

    g.setColour (accent.withAlpha (dragging ? 0.8f : 0.4f));

    if (vertical)
        g.fillRoundedRectangle ((float) w * 0.5f - 1.0f, (float) h * 0.25f, 2.0f, (float) h * 0.5f, 1.0f);
    else
        g.fillRoundedRectangle ((float) w * 0.25f, (float) h * 0.5f - 1.0f, (float) w * 0.5f, 2.0f, 1.0f);
}

void Look::drawProgressBar (juce::Graphics& g, juce::ProgressBar&, int w, int h, double progress, const juce::String& t)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).withSizeKeepingCentre ((float) w, juce::jmin (10.0f, (float) h));
    g.setColour (stepOff);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

    if (progress >= 0.0)
    {
        g.setColour (accent);
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * (float) juce::jlimit (0.0, 1.0, progress)), r.getHeight() * 0.5f);
    }

    if (t.isNotEmpty() && h > 14)
    {
        g.setColour (text);
        g.setFont (ui (12.0f));
        g.drawText (t, 0, 0, w, h, juce::Justification::centred);
    }
}

void Look::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (panel);
    g.fillPath (path);
    g.setColour (line);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

Look& look()
{
    static Look l;
    return l;
}

} // namespace theme

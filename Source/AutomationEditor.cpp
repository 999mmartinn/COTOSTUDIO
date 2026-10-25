#include "AutomationEditor.h"

using namespace song;

static const juce::Colour autoColour (0xff45c4b0);

AutomationEditor::AutomationEditor (EditorState& s) : state (s)
{
    title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    hint.setFont (juce::FontOptions (12.0f));
    hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
    hint.setText ("Click: add a point (Alt: off the grid).  Drag: move.  Right-click a point: delete.  Plays in SONG mode.",
                  juce::dontSendNotification);

    deleteButton.setTooltip ("Delete this automation clip (Ctrl+Z brings it back)");
    deleteButton.onClick = [this]
    {
        auto clip = getClip();

        if (auto* edit = state.getEdit(); edit != nullptr && clip.isValid())
        {
            state.beginAction();
            removeClip (*edit, clip);
            state.selectedAutomation = {};
            refresh();
        }
    };

    for (auto* c : std::initializer_list<juce::Component*> { &title, &hint, &deleteButton })
        addAndMakeVisible (c);

    startTimerHz (20);
}

AutomationEditor::~AutomationEditor()   { stopTimer(); }

juce::ValueTree AutomationEditor::getClip() const
{
    if (auto* edit = state.getEdit())
        for (auto& c : getClips (*edit))
            if (isAutomationClip (c) && c[ids::sid].toString() == state.selectedAutomation)
                return c;

    return {};
}

double AutomationEditor::clipLength() const
{
    auto* edit = state.getEdit();
    auto clip = getClip();
    return edit != nullptr && clip.isValid() ? getClipLength (*edit, clip) : 16.0;
}

juce::Rectangle<float> AutomationEditor::graphArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop (34.0f).reduced (12.0f, 8.0f);
}

float AutomationEditor::xForBeat (double beat) const
{
    auto r = graphArea();
    return r.getX() + (float) (beat / juce::jmax (0.25, clipLength())) * r.getWidth();
}

double AutomationEditor::beatAtX (float x) const
{
    auto r = graphArea();
    return juce::jlimit (0.0, clipLength(), (double) ((x - r.getX()) / juce::jmax (1.0f, r.getWidth())) * clipLength());
}

float AutomationEditor::yForValue (float v) const
{
    auto r = graphArea();
    return r.getBottom() - v * r.getHeight();
}

float AutomationEditor::valueAtY (float y) const
{
    auto r = graphArea();
    return juce::jlimit (0.0f, 1.0f, (r.getBottom() - y) / juce::jmax (1.0f, r.getHeight()));
}

double AutomationEditor::snapBeat (double beat, bool free) const
{
    return free ? beat : std::round (beat / stepBeats) * stepBeats;
}

juce::ValueTree AutomationEditor::pointAt (juce::Point<float> pos) const
{
    for (auto& p : getAutomationPoints (getClip()))
        if (juce::Point<float> (xForBeat (p[ids::b]), yForValue (p[ids::v])).getDistanceFrom (pos) < 8.0f)
            return p;

    return {};
}

juce::String AutomationEditor::valueText (float value01) const
{
    auto* edit = state.getEdit();
    auto* param = edit != nullptr ? findAutomationTarget (*edit, getClip()) : nullptr;

    if (param == nullptr)
        return juce::String (juce::roundToInt (value01 * 100.0f)) + "%";

    return param->valueToString (param->valueRange.convertFrom0to1 (value01));
}

void AutomationEditor::refresh()
{
    auto* edit = state.getEdit();
    auto clip = getClip();

    if (edit != nullptr && clip.isValid())
        title.setText ("Automation: " + describeAutomationTarget (*edit, clip), juce::dontSendNotification);
    else
        title.setText ("Automation", juce::dontSendNotification);

    deleteButton.setVisible (clip.isValid());
    hint.setVisible (clip.isValid());
    repaint();
}

void AutomationEditor::timerCallback()
{
    // The playhead, while the song plays.
    if (state.getSongPlayheadBeat() >= 0.0 && getClip().isValid())
        repaint();
}

void AutomationEditor::resized()
{
    auto r = getLocalBounds().reduced (8, 4);
    auto top = r.removeFromTop (26);
    deleteButton.setBounds (top.removeFromRight (100));
    top.removeFromRight (8);
    title.setBounds (top.removeFromLeft (juce::jmin (380, top.getWidth() / 2)));
    hint.setBounds (top);
}

void AutomationEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff23272d));
    auto* edit = state.getEdit();
    auto clip = getClip();

    if (edit == nullptr || ! clip.isValid())
    {
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (14.0f);
        g.drawFittedText ("No automation clip selected.\n\nRight-click a volume or pan knob (channel rack or mixer) and choose "
                          "\"Create automation clip\", or use \"Automate...\" in the playlist for any knob,\n"
                          "including your plugins'. Then click the clip in the playlist to edit it here.",
                          getLocalBounds().reduced (30), juce::Justification::centred, 6);
        return;
    }

    auto r = graphArea();
    g.setColour (juce::Colour (0xff2a3038));
    g.fillRect (r);

    // Grid: beats and bars, plus 25 / 50 / 75 %.
    const double len = clipLength();

    for (int b = 0; b <= (int) std::ceil (len); ++b)
    {
        g.setColour (b % 4 == 0 ? juce::Colour (0xff4f5864) : juce::Colour (0xff353c45));
        g.drawVerticalLine ((int) xForBeat (b), r.getY(), r.getBottom());

        if (b % 4 == 0 && b < len)
        {
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (10.0f);
            g.drawText (juce::String ((int) ((double) clip[ids::start] + b) / 4 + 1), (int) xForBeat (b) + 3, (int) r.getY() + 1, 40, 12,
                        juce::Justification::centredLeft);
        }
    }

    for (float v : { 0.25f, 0.5f, 0.75f })
    {
        g.setColour (juce::Colour (0xff353c45));
        g.drawHorizontalLine ((int) yForValue (v), r.getX(), r.getRight());
    }

    // The curve (what the knob does) and its points.
    auto points = getAutomationPoints (clip);
    juce::Path path;
    path.startNewSubPath (r.getX(), yForValue (getAutomationValueAt (clip, 0.0)));

    for (auto& p : points)
        if ((double) p[ids::b] <= len)
            path.lineTo (xForBeat (p[ids::b]), yForValue (p[ids::v]));

    path.lineTo (r.getRight(), yForValue (getAutomationValueAt (clip, len)));

    juce::Path fill (path);
    fill.lineTo (r.getRight(), r.getBottom());
    fill.lineTo (r.getX(), r.getBottom());
    fill.closeSubPath();
    g.setColour (autoColour.withAlpha (0.15f));
    g.fillPath (fill);
    g.setColour (autoColour);
    g.strokePath (path, juce::PathStrokeType (2.0f));

    for (auto& p : points)
    {
        if ((double) p[ids::b] > len)
            continue;   // beyond the clip's end (it was shortened): kept, not played

        const juce::Point<float> c (xForBeat (p[ids::b]), yForValue (p[ids::v]));
        g.setColour (p == dragPoint ? juce::Colours::white : autoColour.brighter (0.4f));
        g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (c));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (c), 1.0f);
    }

    // Playhead (song mode)
    const double playBeat = state.getSongPlayheadBeat() - (double) clip[ids::start];

    if (state.getSongPlayheadBeat() >= 0.0 && playBeat >= 0.0 && playBeat <= len)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawVerticalLine ((int) xForBeat (playBeat), r.getY(), r.getBottom());
    }

    // Value under the mouse (or of the point being dragged).
    if (r.contains (hover) || dragPoint.isValid())
    {
        const float v = dragPoint.isValid() ? (float) dragPoint[ids::v] : valueAtY (hover.y);
        const auto text = valueText (v);
        auto at = dragPoint.isValid() ? juce::Point<float> (xForBeat (dragPoint[ids::b]), yForValue (v)) : hover;
        juce::Rectangle<float> box (at.x + 10.0f, at.y - 22.0f, 110.0f, 18.0f);
        box = box.constrainedWithin (r);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (12.0f);
        g.drawText (text, box.reduced (5.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
    }
}

void AutomationEditor::mouseDown (const juce::MouseEvent& e)
{
    auto* edit = state.getEdit();
    auto clip = getClip();
    dragPoint = {};

    if (edit == nullptr || ! clip.isValid() || ! graphArea().expanded (6.0f).contains (e.position))
        return;

    state.beginAction();
    auto hit = pointAt (e.position);

    if (e.mods.isPopupMenu())
    {
        // Keep at least one point, so the clip always says something.
        if (hit.isValid() && getAutomationPoints (clip).size() > 1)
            removeAutomationPoint (*edit, hit);

        repaint();
        return;
    }

    dragPoint = hit.isValid() ? hit
                              : addAutomationPoint (*edit, clip, snapBeat (beatAtX (e.position.x), e.mods.isAltDown()),
                                                    valueAtY (e.position.y));
    repaint();
}

void AutomationEditor::mouseDrag (const juce::MouseEvent& e)
{
    auto* edit = state.getEdit();

    if (edit == nullptr || ! dragPoint.isValid() || dragPoint.getParent() != getClip())
        return;

    moveAutomationPoint (*edit, dragPoint, juce::jlimit (0.0, clipLength(), snapBeat (beatAtX (e.position.x), e.mods.isAltDown())),
                         valueAtY (e.position.y));
    hover = e.position;
    repaint();
}

void AutomationEditor::mouseUp (const juce::MouseEvent&)
{
    dragPoint = {};
    repaint();
}

void AutomationEditor::mouseMove (const juce::MouseEvent& e)
{
    hover = e.position;
    setMouseCursor (pointAt (e.position).isValid() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
    repaint();
}

void AutomationEditor::mouseExit (const juce::MouseEvent&)
{
    hover = { -1.0f, -1.0f };
    repaint();
}

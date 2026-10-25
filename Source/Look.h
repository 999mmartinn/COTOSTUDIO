#pragma once

#include <JuceHeader.h>

/** Small, readable knobs for the rack (FL-style). */
struct KnobLook  : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
        auto size = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (size, size);
        const auto centre = r.getCentre();
        const float radius = size * 0.5f;
        const float angle = startAngle + pos * (endAngle - startAngle);

        g.setColour (juce::Colour (0xff20242a));
        g.fillEllipse (r);

        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, angle, true);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (arc, juce::PathStrokeType (2.5f));

        juce::Path pointer;
        pointer.startNewSubPath (centre);
        pointer.lineTo (centre.getPointOnCircumference (radius - 3.0f, angle));
        g.setColour (juce::Colours::white);
        g.strokePath (pointer, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};

inline KnobLook& knobLook()
{
    static KnobLook look;
    return look;
}

//==============================================================================
/** A slider with an FL-style right-click menu: "Reset" puts it back to its
    default (its double-click value). The reset goes through onDragStart and
    onValueChange like a normal move, so it can be undone and is sent to the
    other PCs. Alt+click and double-click also reset. */
class ResetSlider  : public juce::Slider
{
public:
    using juce::Slider::Slider;

    /** Optional text for the menu, e.g. "0 dB" or "centre". */
    std::function<juce::String()> describeDefault;

    void resetToDefault()
    {
        if (! isDoubleClickReturnEnabled())
            return;

        if (onDragStart)
            onDragStart();

        setValue (getDoubleClickReturnValue(), juce::sendNotificationSync);

        if (onDragEnd)
            onDragEnd();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        rightClick = e.mods.isPopupMenu();

        if (rightClick)
            showMenu();
        else
            juce::Slider::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override   { if (! rightClick) juce::Slider::mouseDrag (e); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! rightClick)
            juce::Slider::mouseUp (e);

        rightClick = false;
    }

private:
    bool rightClick = false;

    void showMenu()
    {
        juce::PopupMenu m;
        auto text = juce::String ("Reset");

        if (describeDefault)
            text << " (" << describeDefault() << ")";

        m.addItem (1, text, isEnabled() && isDoubleClickReturnEnabled());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                         [safe = juce::Component::SafePointer<ResetSlider> (this)] (int result)
                         {
                             if (result == 1 && safe != nullptr)
                                 safe->resetToDefault();
                         });
    }
};


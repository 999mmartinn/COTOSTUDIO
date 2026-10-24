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


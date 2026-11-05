#pragma once

#include "Session.h"
#include "Theme.h"

/**
    Drawing the other people's mouse pointers (2.1, see Session::setMyCursor): an arrow in
    their colour with their name just under it. Views keep a Layer and call update() from
    their ~30 Hz timer: only the small areas around the pointers are repainted.
*/
namespace remotecursors
{
    struct Shown
    {
        juce::Point<float> tip;
        juce::String name;
        juce::Colour colour;
    };

    /** The area a pointer and its name take, around its tip. */
    inline juce::Rectangle<int> areaAt (juce::Point<float> tip)
    {
        return { (int) std::floor (tip.x) - 3, (int) std::floor (tip.y) - 3, 160, 42 };
    }

    inline void draw (juce::Graphics& g, const Shown& s)
    {
        juce::Path arrow;
        arrow.startNewSubPath (0.0f, 0.0f);
        arrow.lineTo (0.0f, 15.0f);
        arrow.lineTo (4.0f, 11.5f);
        arrow.lineTo (6.8f, 17.5f);
        arrow.lineTo (9.4f, 16.4f);
        arrow.lineTo (6.6f, 10.6f);
        arrow.lineTo (11.0f, 10.6f);
        arrow.closeSubPath();
        arrow.applyTransform (juce::AffineTransform::translation (s.tip));

        g.setColour (s.colour);
        g.fillPath (arrow);
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.strokePath (arrow, juce::PathStrokeType (1.0f));

        // The name, just under the pointer.
        const auto font = theme::ui (10.5f, theme::Weight::semibold);
        const auto name = s.name.isNotEmpty() ? s.name : juce::String ("?");
        const float width = juce::jmin (150.0f, juce::GlyphArrangement::getStringWidth (font, name) + 10.0f);
        const juce::Rectangle<float> tag (s.tip.x + 2.0f, s.tip.y + 19.0f, width, 15.0f);
        g.setColour (s.colour);
        g.fillRoundedRectangle (tag, 4.0f);
        g.setColour (s.colour.getPerceivedBrightness() > 0.55f ? juce::Colours::black.withAlpha (0.85f) : juce::Colours::white);
        g.setFont (font);
        g.drawText (name, tag.reduced (5.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, true);
    }

    /** What a view has drawn, so moving pointers repaint only where they were and are. */
    struct Layer
    {
        std::vector<Shown> shown;

        /** Call from the view's timer with where the pointers are now (in the view's
            coordinates; recomputed each time, so zooming and scrolling are followed).
            Repaints only if something changed. */
        void update (juce::Component& view, std::vector<Shown> now)
        {
            auto same = [] (const Shown& a, const Shown& b)
            {
                return a.tip.getDistanceFrom (b.tip) < 0.5f && a.name == b.name && a.colour == b.colour;
            };

            if (now.size() == shown.size() && std::equal (now.begin(), now.end(), shown.begin(), same))
                return;

            for (auto& s : shown)   view.repaint (areaAt (s.tip));
            for (auto& s : now)     view.repaint (areaAt (s.tip));

            shown = std::move (now);
        }

        void paint (juce::Graphics& g) const
        {
            for (auto& s : shown)
                if (g.getClipBounds().intersects (areaAt (s.tip)))
                    draw (g, s);
        }
    };
}

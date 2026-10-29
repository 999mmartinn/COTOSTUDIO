#pragma once

#include <JuceHeader.h>
#include "Look.h"

namespace te = tracktion;

/**
    Shared pieces for COTOSTUDIO's own effects (Limiter, Delay, Reverb):

      - CotoFxPlugin: a te::Plugin whose knobs are automatable parameters stored in the
        song (so they sync to the other PCs and can be automated), made with one call each.
      - The editor kit: ParamKnob (a knob with its name and value), ParamChoice (a row of
        buttons for a stepped setting), ParamToggle, and drawing helpers for the panels.

    The Parametric EQ predates this and keeps its own code.
*/
class CotoFxPlugin  : public te::Plugin
{
public:
    explicit CotoFxPlugin (te::PluginCreationInfo info) : te::Plugin (info) {}

    ~CotoFxPlugin() override
    {
        // The subclass calls notifyListenersOfDeletion() in its own destructor.
        for (auto p : getAutomatableParameters())
            p->detachFromCurrentValue();
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        for (auto& f : fields)
            f->value = v.hasProperty (f->id) ? (float) v[f->id] : f->defaultValue;

        for (auto p : getAutomatableParameters())
            p->updateFromAttachedValue();
    }

    /** All parameters (in the order they were made). */
    const juce::Array<te::AutomatableParameter*>& params() const     { return paramList; }

protected:
    /** Makes a parameter stored in the song under `id`. */
    te::AutomatableParameter* makeParam (const juce::String& id, const juce::String& name,
                                         juce::NormalisableRange<float> range, float defaultValue,
                                         std::function<juce::String (float)> toText)
    {
        auto f = std::make_unique<Field>();
        f->id = id;
        f->defaultValue = defaultValue;
        f->value.referTo (state, f->id, getUndoManager(), defaultValue);

        auto* p = addParam (id, name, range, std::move (toText), {});
        p->attachToCurrentValue (f->value);
        fields.push_back (std::move (f));
        paramList.add (p);
        return p;
    }

    static juce::NormalisableRange<float> skewed (float lo, float hi, float centre, float step = 0.0f)
    {
        juce::NormalisableRange<float> r (lo, hi, step);
        r.setSkewForCentre (centre);
        return r;
    }

    static juce::String dbText (float v)    { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
    static juce::String msText (float v)    { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
    static juce::String pctText (float v)   { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }
    static juce::String hzText (float v)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    }

private:
    struct Field
    {
        juce::Identifier id { "x" };
        float defaultValue = 0.0f;
        juce::CachedValue<float> value;
    };

    std::vector<std::unique_ptr<Field>> fields;
    juce::Array<te::AutomatableParameter*> paramList;
};

//==============================================================================
namespace fxui
{
    using namespace theme::colours;

    /** Upper case that also handles accented letters (JUCE's leaves "ó" as it is on some
        systems: "SATURACIóN"). */
    inline juce::String upper (const juce::String& s)
    {
        auto u = s.toUpperCase();
        const char* pairs[][2] = { { "\xc3\xa1", "\xc3\x81" }, { "\xc3\xa9", "\xc3\x89" }, { "\xc3\xad", "\xc3\x8d" },
                                   { "\xc3\xb3", "\xc3\x93" }, { "\xc3\xba", "\xc3\x9a" }, { "\xc3\xbc", "\xc3\x9c" },
                                   { "\xc3\xb1", "\xc3\x91" } };

        for (auto& p : pairs)
            u = u.replace (juce::String::fromUTF8 (p[0]), juce::String::fromUTF8 (p[1]));

        return u;
    }

    /** Starts one undo step for a user action on a plugin. */
    inline void beginEdit (te::AutomatableParameter& p)     { p.getEdit().getUndoManager().beginNewTransaction(); }

    /** A rounded panel with a title, drawn behind a group of controls. */
    inline void drawSection (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, juce::Colour tint)
    {
        auto r = area.toFloat();
        g.setColour (wellAlt);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (line);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

        if (title.isNotEmpty())
        {
            g.setColour (tint);
            g.fillRoundedRectangle (r.getX() + 10.0f, r.getY() + 9.0f, 3.0f, 10.0f, 1.5f);
            g.setColour (muted);
            g.setFont (theme::ui (10.5f, theme::Weight::bold));
            g.drawText (upper (title), area.withTrimmedLeft (18).withTrimmedTop (5).withHeight (18),
                        juce::Justification::centredLeft, false);
        }
    }

    //==============================================================================
    /** A knob tied to one plugin parameter: name above, value below. Follows changes
        made by other PCs and by automation. Double-click (or right-click > Reset) = default. */
    class ParamKnob  : public juce::Component,
                       private juce::Timer
    {
    public:
        ParamKnob (te::AutomatableParameter& p, const juce::String& label, juce::Colour colour, bool bipolar = false)
            : param (p), name (label)
        {
            knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob.setRange (0.0, 1.0);
            knob.setMouseDragSensitivity (220);
            knob.setColour (juce::Slider::rotarySliderFillColourId, colour);
            knob.setColour (juce::Slider::rotarySliderOutlineColourId, stepOffAlt);
            knob.getProperties().set ("bipolar", bipolar);
            knob.setValue (param.getCurrentNormalisedValue(), juce::dontSendNotification);

            if (auto def = param.getDefaultValue())
            {
                const auto range = param.getValueRange();
                (void) range;
                knob.setDoubleClickReturnValue (true, param.valueRange.convertTo0to1 (*def));
                knob.describeDefault = [&p = param, d = *def] { return p.valueToString (d); };
            }

            knob.onDragStart = [this] { beginEdit (param); };
            knob.onValueChange = [this]
            {
                if (! updating)
                    param.setNormalisedParameter ((float) knob.getValue(), juce::sendNotification);

                repaint();
            };

            setTooltip (label);
            addAndMakeVisible (knob);
            startTimerHz (20);
        }

        void setTooltip (const juce::String& t)          { knob.setTooltip (t); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.setColour (muted);
            g.setFont (theme::ui (10.5f, theme::Weight::semibold));
            g.drawText (upper (name), r.removeFromTop (14), juce::Justification::centred, false);

            g.setColour (knob.isMouseOverOrDragging() ? text : lcdText);
            g.setFont (theme::mono (11.0f));
            g.drawText (param.getCurrentValueAsString(), r.removeFromBottom (14), juce::Justification::centred, false);
        }

        void resized() override
        {
            auto r = getLocalBounds().withTrimmedTop (15).withTrimmedBottom (15);
            knob.setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight())));
        }

    private:
        te::AutomatableParameter& param;
        juce::String name;
        ResetSlider knob;
        bool updating = false;
        float lastShown = -1.0f;

        void timerCallback() override
        {
            const float v = param.getCurrentNormalisedValue();

            if (! knob.isMouseButtonDown() && std::abs (v - (float) knob.getValue()) > 1.0e-6f)
            {
                const juce::ScopedValueSetter<bool> svs (updating, true);
                knob.setValue (v, juce::dontSendNotification);
            }

            if (v != lastShown)
            {
                lastShown = v;
                repaint();
            }
        }
    };

    //==============================================================================
    /** A row of buttons for a stepped parameter (mode, filter type...). */
    class ParamChoice  : public juce::Component,
                         private juce::Timer
    {
    public:
        ParamChoice (te::AutomatableParameter& p, const juce::StringArray& names, juce::Colour colour)
            : param (p), accentColour (colour)
        {
            for (int i = 0; i < names.size(); ++i)
            {
                auto* b = buttons.add (new juce::TextButton (names[i]));
                b->setClickingTogglesState (false);
                b->onClick = [this, i]
                {
                    if (current() == i)
                        return;

                    beginEdit (param);
                    param.setParameter ((float) (param.getValueRange().getStart() + i), juce::sendNotification);
                    refresh();
                };
                b->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                                      | (i < names.size() - 1 ? juce::Button::ConnectedOnRight : 0));
                addAndMakeVisible (b);
            }

            refresh();
            startTimerHz (10);
        }

        int current() const
        {
            return juce::roundToInt (param.getCurrentValue() - param.getValueRange().getStart());
        }

        void resized() override
        {
            auto r = getLocalBounds();
            const int n = buttons.size();

            for (int i = 0; i < n; ++i)
                buttons[i]->setBounds (r.removeFromLeft (i == n - 1 ? r.getWidth() : r.getWidth() / (n - i)));
        }

        std::function<void()> onChange;

        void setTooltip (const juce::String& t)
        {
            for (auto* b : buttons)
                b->setTooltip (t);
        }

    private:
        te::AutomatableParameter& param;
        juce::Colour accentColour;
        juce::OwnedArray<juce::TextButton> buttons;
        int shown = -1;

        void refresh()
        {
            const int c = current();

            if (c == shown)
                return;

            shown = c;

            for (int i = 0; i < buttons.size(); ++i)
            {
                buttons[i]->setColour (juce::TextButton::buttonColourId, i == c ? accentColour : well);
                buttons[i]->setColour (juce::TextButton::textColourOffId, i == c ? accentText : muted);
            }

            if (onChange)
                onChange();
        }

        void timerCallback() override   { refresh(); }
    };

    //==============================================================================
    /** An on/off button for a 0/1 parameter. */
    class ParamToggle  : public juce::TextButton,
                         private juce::Timer
    {
    public:
        ParamToggle (te::AutomatableParameter& p, const juce::String& label, juce::Colour colour)
            : juce::TextButton (label), param (p), accentColour (colour)
        {
            onClick = [this]
            {
                beginEdit (param);
                param.setParameter (isOn() ? 0.0f : 1.0f, juce::sendNotification);
                refresh();
            };

            refresh();
            startTimerHz (10);
        }

        bool isOn() const     { return param.getCurrentValue() >= 0.5f; }
        std::function<void()> onChange;

    private:
        te::AutomatableParameter& param;
        juce::Colour accentColour;
        int shown = -1;

        void refresh()
        {
            const int on = isOn() ? 1 : 0;

            if (on == shown)
                return;

            shown = on;
            setColour (juce::TextButton::buttonColourId, on ? accentColour : well);
            setColour (juce::TextButton::textColourOffId, on ? accentText : muted);

            if (onChange)
                onChange();
        }

        void timerCallback() override   { refresh(); }
    };
}

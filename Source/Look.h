#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/** Knobs use the app's look (Nocturno), like everything else. */
inline juce::LookAndFeel& knobLook()     { return theme::look(); }

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

    /** If set, the right-click menu offers "Create automation clip" (FL-style). */
    std::function<void()> onCreateAutomation;

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
        auto text = TRANS ("Reset");

        if (describeDefault)
            text << " (" << describeDefault() << ")";

        m.addItem (1, text, isEnabled() && isDoubleClickReturnEnabled());

        if (onCreateAutomation)
            m.addItem (2, TRANS ("Create automation clip"));

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                         [safe = juce::Component::SafePointer<ResetSlider> (this)] (int result)
                         {
                             if (safe == nullptr)
                                 return;

                             if (result == 1)
                                 safe->resetToDefault();
                             else if (result == 2 && safe->onCreateAutomation)
                                 safe->onCreateAutomation();
                         });
    }
};

//==============================================================================
/** A TextButton that remembers whether it was pressed with the right mouse button.
    (On Windows the button is already released when onClick runs, so the global
    modifier state can't tell a right-click from a left-click at that point.) */
class MenuButton  : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    bool wasRightClick() const noexcept     { return rightClick; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        rightClick = e.mods.isPopupMenu();
        juce::TextButton::mouseDown (e);
    }

private:
    bool rightClick = false;
};


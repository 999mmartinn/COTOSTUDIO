#pragma once

#include <JuceHeader.h>
#include "EditSync.h"
#include "Look.h"
#include "ParametricEQEditor.h"
#include "LimiterEditor.h"
#include "DelayEditor.h"
#include "ReverbEditor.h"
#include "SamplerEditor.h"

#if JUCE_WINDOWS
 #include "PluginKeysWin.h"
#endif

namespace te = tracktion;

namespace pluginwindows
{
    /** Keys a plugin window doesn't use (Space = play / stop, Ctrl+S...) go to the main
        window, so the song can be started and stopped while you tweak a plugin (set by it). */
    inline std::function<bool (const juce::KeyPress&)> forwardKey;
}

//==============================================================================
/** Simple sliders for a built-in Tracktion plugin (they have no GUI of their own). */
class GenericPluginPanel  : public juce::Component,
                            private juce::Timer
{
public:
    explicit GenericPluginPanel (te::Plugin& p) : plugin (p)
    {
        auto params = plugin.getAutomatableParameters();

        for (int i = 0; i < juce::jmin (40, params.size()); ++i)
        {
            te::AutomatableParameter::Ptr ap (params[i]);
            auto* row = rows.add (new Row());
            row->label.setText (ap->getParameterName(), juce::dontSendNotification);
            row->label.setColour (juce::Label::textColourId, theme::colours::muted);
            row->label.setFont (theme::ui (12.0f, theme::Weight::medium));
            row->slider.setColour (juce::Slider::trackColourId, theme::colours::accent);
            row->slider.setColour (juce::Slider::textBoxTextColourId, theme::colours::text);
            row->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 90, 22);
            row->slider.setRange (0.0, 1.0);
            row->slider.textFromValueFunction = [ap] (double v)
            {
                auto range = ap->getValueRange();
                return ap->valueToString (range.getStart() + (float) v * range.getLength());
            };
            row->slider.setValue (ap->getCurrentNormalisedValue(), juce::dontSendNotification);

            if (auto def = ap->getDefaultValue())
            {
                auto range = ap->getValueRange();
                row->slider.setDoubleClickReturnValue (true, (*def - range.getStart()) / juce::jmax (0.0001f, range.getLength()));
                row->slider.describeDefault = [ap, d = *def] { return ap->valueToString (d); };
            }

            row->slider.onDragStart = [&p = plugin] { p.edit.getUndoManager().beginNewTransaction(); };
            row->slider.onValueChange = [ap, s = &row->slider]
            {
                ap->setNormalisedParameter ((float) s->getValue(), juce::sendNotification);
            };
            row->param = ap;
            content.addAndMakeVisible (row->label);
            content.addAndMakeVisible (row->slider);
        }

        viewport.setViewedComponent (&content, false);
        addAndMakeVisible (viewport);
        setSize (420, juce::jmin (520, 30 * rows.size() + 10));
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override   { g.fillAll (theme::colours::panel); }

    void resized() override
    {
        viewport.setBounds (getLocalBounds().reduced (6));
        content.setSize (getWidth() - viewport.getScrollBarThickness(), 30 * rows.size() + 10);

        int y = 5;

        for (auto* r : rows)
        {
            r->label.setBounds (8, y, 150, 26);
            r->slider.setBounds (160, y, content.getWidth() - 168, 26);
            y += 30;
        }
    }

private:
    struct Row
    {
        juce::Label label;
        ResetSlider slider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        te::AutomatableParameter::Ptr param;
    };

    te::Plugin& plugin;
    juce::OwnedArray<Row> rows;
    juce::Component content;
    juce::Viewport viewport;

    // Follow changes made by other peers.
    void timerCallback() override
    {
        for (auto* r : rows)
            if (! r->slider.isMouseButtonDown())
                r->slider.setValue (r->param->getCurrentNormalisedValue(), juce::dontSendNotification);
    }
};

//==============================================================================
/** A window showing a plugin's own editor (VST3) or the generic panel (built-in). */
class PluginEditorWindow  : public juce::DocumentWindow
{
public:
    explicit PluginEditorWindow (te::Plugin& p)
        : DocumentWindow (p.getName(), theme::colours::panel, DocumentWindow::closeButton),
          plugin (p)
    {
        setUsingNativeTitleBar (true);

        if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&plugin))
        {
            if (auto* instance = ext->getAudioPluginInstance())
            {
                content.reset (instance->createEditorAndMakeActive());

                if (content == nullptr)
                    content = std::make_unique<juce::GenericAudioProcessorEditor> (*instance);
            }
        }

        if (auto* eq = dynamic_cast<ParametricEQPlugin*> (&plugin))
            content = std::make_unique<ParametricEQEditor> (*eq);
        else if (auto* limiter = dynamic_cast<LimiterPlugin*> (&plugin))
            content = std::make_unique<LimiterEditor> (*limiter);
        else if (auto* delay = dynamic_cast<DelayPlugin*> (&plugin))
            content = std::make_unique<DelayEditor> (*delay);
        else if (auto* reverb = dynamic_cast<ReverbPlugin*> (&plugin))
            content = std::make_unique<ReverbEditor> (*reverb);
        else if (auto* sampler = dynamic_cast<SamplerPlugin*> (&plugin))
            content = std::make_unique<SamplerEditor> (*sampler);

        if (content == nullptr)
            content = std::make_unique<GenericPluginPanel> (plugin);

        setContentNonOwned (content.get(), true);
        setResizable (false, false);
        setTopLeftPosition (plugin.windowState->choosePositionForPluginWindow());
        setVisible (true);
        toFront (true);

       #if JUCE_WINDOWS
        // A plugin's own editor (VST3) gets the keys before us: Space there plays / stops too.
        nativeHandle = getWindowHandle();
        pluginkeys::onSpace = []
        {
            juce::MessageManager::callAsync ([]
            {
                if (pluginwindows::forwardKey)
                    pluginwindows::forwardKey (juce::KeyPress (juce::KeyPress::spaceKey));
            });
        };
        pluginkeys::watch (nativeHandle);
       #endif
    }

    /** Keys this window's controls don't use go to the main window (Space plays / stops). */
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (pluginwindows::forwardKey && pluginwindows::forwardKey (key))
            return true;

        return DocumentWindow::keyPressed (key);
    }

    ~PluginEditorWindow() override
    {
       #if JUCE_WINDOWS
        pluginkeys::unwatch (nativeHandle);
       #endif

        // Writes the plugin's full state (preset etc.) into the song and sends it
        // to the other peers.
        collab::EditSync::ScopedPluginStateCommit commit;
        plugin.edit.flushPluginStateIfNeeded (plugin);
        clearContentComponent();
        content.reset();
    }

    void closeButtonPressed() override
    {
        plugin.windowState->closeWindowExplicitly();
    }

private:
    te::Plugin& plugin;
    std::unique_ptr<juce::Component> content;
    void* nativeHandle = nullptr;
};

//==============================================================================
class AppUIBehaviour  : public te::UIBehaviour
{
public:
    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& state) override
    {
        if (auto* ws = dynamic_cast<te::Plugin::WindowState*> (&state))
        {
            if (auto* ext = dynamic_cast<te::ExternalPlugin*> (&ws->plugin))
                if (ext->getAudioPluginInstance() == nullptr)
                    return {};   // not installed on this PC

            return std::make_unique<PluginEditorWindow> (ws->plugin);
        }

        return {};
    }
};

#pragma once

#include "EditorState.h"

/**
    FL-style mixer: the master strip, then one strip per insert.
      - Each strip: name, level meter, volume fader, pan knob, mute, and where
        it sends its sound ("-> Master" / "-> 2").
      - Click a strip to select it; its effect slots show on the right:
        click an empty slot to add an effect, click a filled one to open it,
        right-click for bypass / move / replace / remove.
      - "+ Insert" adds an insert. Channels are routed to inserts from the
        small box on each channel-rack row.
*/
class Mixer  : public juce::Component,
               private juce::Timer
{
public:
    Mixer (te::Engine&, EditorState&);
    ~Mixer() override;

    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int stripWidth = 100, stripGap = 6, slotsWidth = 260, numSlots = 10;

    /** Shows a menu of effects (built-in + your VST3s). Calls back with either a
        built-in type name or a plugin description; the caller creates the effect
        in the current song (it may have been replaced while the menu was open). */
    using EffectChoice = std::function<void (const juce::String& builtInType, const juce::PluginDescription* external)>;
    static void showEffectMenu (te::Engine&, juce::Component* target, EffectChoice onChosen);

    /** Shows "route to" choices for a track (Master, inserts, new insert). */
    static void showRouteMenu (EditorState&, te::AudioTrack& source, juce::Component* target);

    /** The mixer can live in its own window (e.g. on a second monitor). */
    std::function<void()> onDetachToggle, onFullScreenToggle;
    void setDetached (bool);

private:
    class Strip;
    class Slots;

    te::Engine& engine;
    EditorState& state;
    juce::OwnedArray<Strip> strips;
    juce::StringArray stripIDs;
    juce::Component stripsContent;
    struct StripsViewport  : public juce::Viewport
    {
        std::function<void()> onScroll;
        void visibleAreaChanged (const juce::Rectangle<int>&) override   { if (onScroll) onScroll(); }
    };

    StripsViewport stripsViewport;
    juce::TextButton addInsertButton { "+ Insert" }, detachButton { "Own window" }, fullScreenButton { "Full screen" };
    std::unique_ptr<Slots> slots;

    void layoutStrips();
    void timerCallback() override;
};

#pragma once

/** Windows only: Space pressed inside a plugin's own editor (a VST3 window, which gets
    the keys before COTOSTUDIO does) plays / stops the song instead, like in FL, and Tab
    closes the window - unless a text field in the plugin is being typed in. Kept apart from JUCE so <windows.h>
    doesn't meet the JUCE headers. */
namespace pluginkeys
{
    /** Starts / stops watching one of our plugin windows (its top-level window handle). */
    void watch (void* topLevelWindow);
    void unwatch (void* topLevelWindow);

    /** Called (on the UI thread, from inside the key hook) when Space / Tab was taken. */
    extern void (*onSpace)();
    extern void (*onTab)();
}

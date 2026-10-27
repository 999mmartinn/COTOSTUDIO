#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

/**
    Imports an FL Studio project (.flp), typically a recording template, into a
    COTOSTUDIO song: mixer inserts with their names, faders, routing, sends and
    effect chains (third-party VST3 effects with their saved settings), the tempo
    and the names of the playlist tracks.

    The .flp is read as data only, from what the file itself contains (no FL code).
    Layout, as written by FL Studio 20-26:
      "FLhd" <u32 6> <u16 format> <u16 channels> <u16 ppq>
      "FLdt" <u32 size> events...
    Each event is an ID byte followed by 1 (IDs 0-63), 2 (64-127) or 4 (128-191)
    bytes, or (192-255) a 7-bit varint length and that many bytes.

    The mixer part, per insert (0 = master), in order:
      header (154 input, 147 output, 149 colour, 204 name, 236 flags...),
      effect slots: 201 plugin kind ("Fruity Wrapper" = VST), 203 display name,
                    212 slot info, 213 plugin data, closed by 98 = slot number,
      235 = routing: one byte per destination insert (1 = routed; index 0 = master).
    Then 225: the mixer's parameter table, 12-byte records:
      u32 0, u8 what, u8 kind, u16 0x7000 + insert * 64 + sub, i32 value
      kind 0x1f: insert values - what 0: slot <sub> on/off, 1: slot mix,
                 0xc0 volume (12800 = 0 dB, 16000 = max), 0xc1 pan (+-6400)
      kind 0x20: route level from <insert> to insert <what> (12800 = 0 dB).
    A VST wrapper's data (213) is u32 version + records <u32 id><u64 size><bytes>:
      50 kind (7 = VST3, 0 = VST2), 51 VST2 ID, 52 VST3 class ID (16 bytes),
      54 name, 55 file, 56 vendor, 53 state = 80-byte header + records
      (3 = VST3 component state, 2 = controller state, 4 = FL's parameter list).
*/
namespace flimport
{
    struct Effect
    {
        int slot = 0;
        juce::String kind;             // "Fruity Wrapper" for VST plugins, else FL's own effect ("Fruity Limiter")
        juce::String displayName;      // the name FL showed (often the plugin's name; can be renamed: "comp")
        juce::String pluginName;       // a VST's own name ("FabFilter Pro-Q 3", "WaveShell1-VST3 14.12_x64")
        juce::String vendor, file;
        bool isVST = false, isVST3 = false;
        bool missingInFL = false;      // FL couldn't load it either: only its name was saved
        juce::MemoryBlock vst3ClassID;                     // 16 bytes, VST3 only
        juce::MemoryBlock componentState, controllerState; // VST3 only
        bool enabled = true;
        double mix = 1.0;              // wet amount 0..1 (FL's per-slot mix knob)

        /** What to call it in messages: "C4 Stereo", "Fruity Limiter". */
        juce::String describe() const;
    };

    struct Route
    {
        int to = 0;                    // FL insert number (0 = master)
        double db = 0.0;               // level (minus infinity = closed)
        bool closed = false;           // level 0 in FL
    };

    struct Insert
    {
        int number = 0;                // FL insert number (0 = master)
        juce::String name;             // "" = never named in FL
        double volumeDb = 0.0;
        double pan = 0.0;              // -1..1
        juce::Array<Effect> effects;
        juce::Array<Route> routes;

        bool routesTo (int destination) const;
        /** A name for messages and new inserts: its own, or "Insert <n>" / "Master". */
        juce::String displayName() const;
    };

    struct Project
    {
        juce::String flVersion;
        double bpm = 0.0;              // 0 = not found
        int numerator = 4, denominator = 4;
        juce::Array<Insert> inserts;   // index = FL insert number (0 = master)
        juce::StringArray trackNames;  // playlist track names, index = track - 1 ("" = unnamed)

        /** True if the insert has a name, effects, or routing other than straight to the master. */
        bool isUsed (int insertNumber) const;
        /** The inserts worth importing (used ones), in FL order, without the master. */
        juce::Array<int> getUsedInserts() const;
    };

    /** Reads an .flp. Returns an error message, or "" when it worked. */
    juce::String parse (const juce::MemoryBlock& fileData, Project&);
    juce::String parse (const juce::File&, Project&);

    /** FL's fader / route value (12800 = 0 dB) in dB; -100 for 0 (off). FL's exact curve
        isn't published: this matches its ends (0 dB at 12800, +5.6 dB at 16000). */
    double flLevelToDb (int value);

    //==============================================================================
    struct Options
    {
        juce::Array<int> inserts;      // FL insert numbers to bring over (the master always comes)
        bool namesOnPlaylist = true;   // the playlist track names
        bool tempo = true;
        bool skipOffEffects = false;   // leave out effects that were switched off in FL (less CPU and memory)
    };

    struct Report
    {
        int insertsMade = 0, effectsLoaded = 0, effectsWithSettings = 0, sendsMade = 0;
        juce::StringArray notInstalled;     // "C4 Stereo (ADLIB MIX)"
        juce::StringArray notAvailable;     // FL effects COTOSTUDIO doesn't have yet
        juce::StringArray defaultSettings;  // loaded, but without FL's settings
        juce::StringArray leftOutOff;       // off in FL, left out (Options::skipOffEffects)
        juce::StringArray notes;            // anything else worth knowing

        juce::String toText() const;
    };

    /** Finds the installed plugin matching an FL effect (VST3 class ID, then names). */
    const juce::PluginDescription* findPlugin (const juce::Array<juce::PluginDescription>& known, const Effect&);

    /** Builds the chosen parts of the FL project into an (empty) song: inserts,
        effects, routing, sends, faders, tempo, playlist track names. Message thread. */
    Report apply (te::Edit&, const Project&, const Options&);

    /** A readable summary of what an .flp holds (developer option --flp-info=<file>). */
    juce::String describeFile (const juce::File&);

    /** Imports into a throw-away song and describes the result: the report and every insert
        with its route, sends, fader and effects (developer option --flp-dry-run).
        Arguments used: --flp-skip=Name|Name (inserts to leave out), --flp-skip-off. */
    juce::String dryRun (te::Engine&, const juce::File&, const juce::StringArray& args);

    /** The state Tracktion stores for a VST3 plugin, made from FL's saved VST3 states. */
    juce::MemoryBlock makeVST3State (const juce::MemoryBlock& component, const juce::MemoryBlock& controller);
}

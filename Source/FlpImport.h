#pragma once

#include <JuceHeader.h>
#include "AssetStore.h"

namespace te = tracktion;

/**
    Imports an FL Studio project (.flp) into a COTOSTUDIO song: a recording template's
    mixer inserts with their names, faders, routing, sends and effect chains (third-party
    VST3 effects with their saved settings), the tempo and the names of the playlist
    tracks - and a beat: its channels (sampler sounds, VST3 instruments with their
    settings), patterns with their notes, and the playlist (pattern and audio clips).

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
    FL's own effects (213 in an effect slot) save a list of numbers, 32-bit each, first a
    version (1.11, worked out from projects with every knob at its default, minimum and
    maximum, and checked against a screenshot of the same delay):
      Fruity Delay 3 (version 1, 27 values): input, model (mono, stereo, ping pong, off),
        tempo sync, keep pitch, time (1/48 of a step), smoothing, offset (500 = centre),
        stereo, mod rate, mod time, mod cutoff, diffusion level, spread, distortion type
        (limit, sat), feedback level, filter (LP, HP, BP, off), cutoff, resonance,
        distortion level, knee, symmetry, sample rate (60000 = max), bits, wet, tone
        (0 = centre), dry. Knobs go 0..6000.
      Fruity Reeverb 2 (version 10001, 15 values, then 2 bytes: tempo, mid / side):
        low cut (Hz), high cut (100 Hz), predelay (1/48 of a step), size (%), diffusion (%),
        decay (0.1 s), high damping (100 Hz, max = off), bass (x0.01), crossover (Hz),
        modulation (0..64), dry, early reflections, wet (128 = 100 %), mod speed (0..2048),
        stereo separation.
      Fruity Parametric EQ 2 (version 8): 7 band levels (0.01 dB), 7 frequencies (0..65536,
        20 Hz..20 kHz on a log scale), 7 widths, 7 types (0 off, 1 low pass, 2 band pass,
        3 high pass, 4 notch, 5 low shelf, 6 peaking, 7 high shelf), 7 slopes, main level
        (0.01 dB); then the same again (FL's compare slot).
    The beat (1.10):
      channels: 64 = channel index (a new channel), 21 type (0 sampler, 2 plugin,
        3 layer, 4 audio clip, 5 automation), 201 plugin kind ("Fruity Wrapper" = VST),
        203 name, 196 sound file, 213 plugin data, 104 mixer insert, 219 levels
        (i32 pan 0..12800, i32 volume, 10000 = FL's default), 218 envelopes (5 records:
        panning, volume, mod X, mod Y, pitch; u32 #1 = -1 when on).
      patterns: 65 = pattern number, then 224 notes (24-byte records: u32 position,
        u16 flags, u16 channel, u32 length, u16 key (60 = C5), u16 group, u8 fine
        (120 = 0), u8, u8 release (64), u8, u8 pan (64 = centre), u8 velocity, u8 x2),
        193 name. Times in ticks: the header's PPQ per beat.
      playlist: 233 = items (88 bytes in FL 21+, 60 or 32 before): u32 position, u16 20480,
        u16 item (>= 20480: pattern item - 20480, else a channel: audio / automation),
        u32 length, u16 499 - track, u16 group, 8 bytes, then the trim: patterns i32 start
        / end ticks into the pattern (-1 = none), audio f32 start / end in ms of the file.
    A VST wrapper's data (213) is u32 version + records <u32 id><u64 size><bytes>:
      50 kind (7 = VST3 effect, 8 = VST3 instrument, 0 / 4 = VST2), 51 VST2 ID, 52 VST3 class ID (16 bytes),
      54 name, 55 file, 56 vendor, 53 state = 80-byte header + records
      (3 = VST3 component state, 2 = controller state, 4 = FL's parameter list); for a VST2:
      i32 -9, i32, u8, u32 chunk size, 8 bytes, then the plugin's own chunk.
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
        int vst2ID = 0;                // a VST2's own ID (its "unique ID")
        juce::MemoryBlock vst3ClassID;                     // 16 bytes, VST3 only
        juce::MemoryBlock componentState, controllerState; // VST3; or (stateFromVST2) a VST2's own chunk
        bool stateFromVST2 = false;    // FL used the VST2: its chunk (for the VST2 here, or else the VST3)
        juce::MemoryBlock flData;      // one of FL's own effects: its settings, as FL saved them (213)
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

    /** A channel of FL's channel rack. */
    struct Channel
    {
        enum Type { sampler = 0, generator = 2, layer = 3, audioClip = 4, automation = 5 };   // generator: an instrument plugin

        int index = 0;                 // FL's channel number (notes and playlist items refer to it)
        int type = sampler;
        juce::String name, kind;       // kind: "Fruity Wrapper" (a VST), "" (sampler), or FL's own ("3x Osc", "FLEX")
        juce::String samplePath;       // as FL saved it ("%FLStudioFactoryData%\\Projects\\...\\808.wav")
        juce::File sampleFile;         // found on this PC (see findSamples)
        Effect plugin;                 // a VST instrument (kind "Fruity Wrapper")
        int insert = -1;               // FL mixer insert it plays through (0 = master), -1 = not said
        double volume = 1.0;           // FL's volume (1 = its default 78%)
        double pan = 0.0;              // -1..1
        bool volumeEnvelope = false;   // FL's volume envelope on: the sound ends with its note

        bool needsSample() const noexcept       { return (type == sampler || type == audioClip) && samplePath.isNotEmpty(); }
    };

    struct Note
    {
        int channel = 0;
        double beat = 0.0, length = 0.0;
        int key = 60, velocity = 100;           // key: 60 = C5 (the same here)
        int pan = 0, fine = 0, release = 64;    // -100..100, cents, 0..127
    };

    struct Pattern
    {
        int number = 0;                // FL's pattern number (1-based)
        juce::String name;
        std::vector<Note> notes;
    };

    /** An item on the playlist (of the arrangement FL had open). */
    struct Item
    {
        int track = 0;                 // 0-based playlist track
        double start = 0.0, length = 0.0;   // beats
        int pattern = 0;               // FL pattern number, 0 = a channel item
        int channel = -1;              // FL channel (audio or automation clip)
        double patternOffset = 0.0;    // beats into the pattern it starts at
        double audioOffset = 0.0;      // seconds into the audio file it starts at
    };

    struct Project
    {
        juce::String flVersion;
        double bpm = 0.0;              // 0 = not found
        int numerator = 4, denominator = 4;
        juce::Array<Insert> inserts;   // index = FL insert number (0 = master)
        juce::StringArray trackNames;  // playlist track names, index = track - 1 ("" = unnamed)
        juce::Array<juce::Colour> trackColours;   // same index; transparent = FL's default grey

        // The beat
        int ppq = 96;                  // ticks per beat
        std::vector<Channel> channels;
        std::vector<Pattern> patterns;
        std::vector<Item> items;

        bool hasBeat() const noexcept  { return ! patterns.empty() || ! items.empty(); }
        const Channel* findChannel (int index) const;
        /** Mixer inserts FL's channels play through (part of the beat's setup). */
        juce::Array<int> getChannelInserts() const;
        /** The sound files the beat needs, or (foundOnly) how many of them were found. */
        int countSamples (bool foundOnly) const;
        /** Looks for the beat's sound files: where FL saved them (with FL's folders worked
            out), then by name next to the .flp and in these folders (and theirs). */
        void findSamples (const juce::File& flpFile, const juce::Array<juce::File>& moreFolders = {});

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
        bool beat = true;              // channels, patterns and the playlist (if the .flp has them)
        AssetStore* assets = nullptr;  // where the beat's sounds go (needed for them)
    };

    struct Report
    {
        int insertsMade = 0, effectsLoaded = 0, effectsWithSettings = 0, sendsMade = 0;
        juce::StringArray notInstalled;     // "C4 Stereo (ADLIB MIX)"
        juce::StringArray notAvailable;     // FL effects COTOSTUDIO doesn't have yet
        juce::StringArray defaultSettings;  // loaded, but without FL's settings
        juce::StringArray leftOutOff;       // off in FL, left out (Options::skipOffEffects)
        juce::StringArray notes;            // anything else worth knowing
        int channelsMade = 0, patternsMade = 0, clipsMade = 0, notesMade = 0;
        juce::StringArray missingSounds;        // "808.wav (Sad ass 808)"
        juce::StringArray instrumentsMissing;   // VSTs not installed, FL's own instruments

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

    /** Gives one of our effects the settings of the FL effect it stands in for (Fruity
        Delay 3, Fruity Reeverb 2, Fruity Parametric EQ 2). Returns false if the FL data
        isn't in a layout it knows (the effect then keeps its defaults). */
    bool applyFlSettings (te::Plugin& ours, const Effect& fl, double bpm);

    /** The state Tracktion stores for a VST2 plugin (an .fxb bank chunk), made from the
        plugin's own chunk as FL saved it. */
    juce::MemoryBlock makeVST2State (const juce::MemoryBlock& chunk, int vst2ID);

    /** The state Tracktion stores for a VST3 plugin, made from FL's saved VST3 states. */
    juce::MemoryBlock makeVST3State (const juce::MemoryBlock& component, const juce::MemoryBlock& controller);
}

#pragma once

#include "SongModel.h"

/**
    Files from your own folders (the sound browser, or dragged in from Explorer):
    what kind they are, and reading MIDI files into notes.
*/
namespace fileimport
{
    /** Sound files this app can read. */
    inline const char* audioExtensions = "wav;aif;aiff;flac;mp3;ogg";
    inline const char* midiExtensions = "mid;midi";

    inline bool isAudioFile (const juce::File& f)    { return f.existsAsFile() && f.hasFileExtension (audioExtensions); }
    inline bool isMidiFile (const juce::File& f)     { return f.existsAsFile() && f.hasFileExtension (midiExtensions); }

    /** Things dragged around inside the app are "kind:id"; files from the browser are "file:<full path>". */
    inline juce::String dragDescriptionFor (const juce::File& f)   { return "file:" + f.getFullPathName(); }

    inline juce::File draggedFile (const juce::var& description)
    {
        const auto text = description.toString();
        return text.startsWith ("file:") ? juce::File (text.fromFirstOccurrenceOf ("file:", false, false)) : juce::File();
    }

    struct MidiResult
    {
        juce::Array<song::NoteSpec> notes;   // in beats from the file's start
        double lengthBeats = 0.0;            // end of the last note
        juce::String error;
        bool ok() const                      { return error.isEmpty(); }
    };

    /** Every note of every track in a MIDI file. Files timed in seconds (rare) are
        read at the given tempo. */
    MidiResult readMidiFile (const juce::File&, double bpmForSecondsFiles = 120.0);

    /** Puts a MIDI file's notes on a channel in a pattern, replacing that channel's
        notes there (like dropping it on the channel in FL). channelID "" (or an audio
        channel) = a new Synth channel named after the file; channelID is set to the
        channel used. Returns what happened, for the log. */
    juce::String applyMidiFile (te::Edit&, juce::ValueTree pattern, const juce::File&, juce::String& channelID);
}

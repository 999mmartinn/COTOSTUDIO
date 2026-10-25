#include "FileImport.h"

namespace fileimport
{

MidiResult readMidiFile (const juce::File& file, double bpmForSecondsFiles)
{
    MidiResult result;
    juce::FileInputStream in (file);
    juce::MidiFile midi;

    if (! in.openedOk() || ! midi.readFrom (in))
    {
        result.error = "Can't read " + file.getFileName() + " (not a MIDI file, or damaged)";
        return result;
    }

    const short timeFormat = midi.getTimeFormat();
    double ticksPerBeat = timeFormat > 0 ? (double) timeFormat : 0.0;

    if (ticksPerBeat <= 0.0)
    {
        // Timed in seconds (SMPTE): turn seconds into beats at the given tempo.
        midi.convertTimestampTicksToSeconds();
        ticksPerBeat = 0.0;
    }

    auto toBeats = [&] (double time)
    {
        return ticksPerBeat > 0.0 ? time / ticksPerBeat : time * bpmForSecondsFiles / 60.0;
    };

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*midi.getTrack (t));
        seq.updateMatchedPairs();

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            auto* event = seq.getEventPointer (i);
            const auto& m = event->message;

            if (! m.isNoteOn())
                continue;

            const double start = toBeats (m.getTimeStamp());
            double end = event->noteOffObject != nullptr ? toBeats (event->noteOffObject->message.getTimeStamp())
                                                         : start + song::stepBeats;

            if (end <= start)
                end = start + song::stepBeats;

            // Tidy up the tiny timing errors MIDI files often have (1/256 of a beat).
            auto tidy = [] (double b) { return std::round (b * 256.0) / 256.0; };

            song::NoteSpec n;
            n.pitch = m.getNoteNumber();
            n.velocity = juce::jlimit (1, 127, (int) m.getVelocity());
            n.beat = juce::jmax (0.0, tidy (start));
            n.length = juce::jmax (song::stepBeats / 4.0, tidy (end) - n.beat);
            result.notes.add (n);
            result.lengthBeats = juce::jmax (result.lengthBeats, n.beat + n.length);
        }
    }

    if (result.notes.isEmpty())
        result.error = file.getFileName() + " has no notes";

    return result;
}

juce::String applyMidiFile (te::Edit& edit, juce::ValueTree pattern, const juce::File& file, juce::String& channelID)
{
    const double bpm = edit.tempoSequence.getTempo (0) != nullptr ? edit.tempoSequence.getTempo (0)->getBpm() : 120.0;
    auto midi = readMidiFile (file, bpm);

    if (! midi.ok())
        return midi.error;

    if (! pattern.isValid())
        return "There is no pattern to put the notes in";

    te::AudioTrack* track = channelID.isNotEmpty() ? song::findChannel (edit, channelID) : nullptr;

    if (track != nullptr && song::isAudioChannel (*track))
        track = nullptr;   // audio channels don't play notes

    if (track == nullptr)
    {
        track = song::addChannel (edit, song::InstrumentKind::synth);

        if (track == nullptr)
            return "Couldn't add a channel for " + file.getFileName();

        track->setName (song::channelNameForFile (file.getFileName()));
    }

    channelID = song::getChannelID (*track);
    const int added = song::setChannelNotes (edit, pattern, channelID, midi.notes);

    juce::String text;
    text << added << (added == 1 ? " note" : " notes") << " from " << file.getFileName() << " on "
         << track->getName() << " (" << pattern[song::ids::name].toString() << ")";

    if (added < midi.notes.size())
        text << " - " << (midi.notes.size() - added) << " notes after bar 8 left out (a pattern holds up to 8 bars)";

    return text;
}

} // namespace fileimport

#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

/**
    Plays a sound file from your folders straight to your speakers, to hear it
    before using it (the sound browser's click-to-preview). Only you hear it:
    nothing is added to the song and nothing is sent to anyone.

    It runs next to the engine on the same sound card (an extra callback on the
    audio device), so it works whether the song is playing or not.
*/
class SamplePreviewer  : public juce::ChangeBroadcaster
{
public:
    explicit SamplePreviewer (te::Engine&);
    ~SamplePreviewer() override;

    /** Starts playing this file from the beginning (stops what was playing).
        False if it isn't an audio file this app can read. */
    bool play (const juce::File&);
    void stop();

    bool isPlaying() const;
    juce::File getFile() const                       { return file; }
    double getLengthSeconds() const;
    double getPositionSeconds() const;

    void setGain (float);
    float getGain() const                            { return gain; }

    juce::AudioFormatManager& getFormats()           { return formats; }

private:
    te::Engine& engine;
    juce::AudioFormatManager formats;
    juce::TimeSliceThread readAhead { "Preview read-ahead" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> reader;
    juce::AudioSourcePlayer player;
    juce::File file;
    float gain = 0.8f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePreviewer)
};

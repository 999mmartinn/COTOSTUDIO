#pragma once

#include <JuceHeader.h>

/** MP3 export with the MP3 encoder that comes with Windows (Media Foundation,
    Windows 8 and newer), so no extra encoder has to be installed or shipped. */
namespace mp3
{
    bool isAvailable();

    /** Encodes a WAV file to MP3 (constant bit rate: 128, 192, 256 or 320 kbps).
        Returns an error text, or "" if it worked. Stops early when cancel turns true. */
    juce::String encodeWavFile (const juce::File& wav, const juce::File& mp3File, int kbps, const std::atomic<bool>& cancel);
}

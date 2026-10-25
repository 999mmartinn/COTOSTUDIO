#pragma once

// Windows only, and on purpose without JUCE: Windows' headers and JUCE's names clash.
#include <cstdint>
#include <string>

namespace mfmp3
{
    struct Writer;

    /** True if this Windows has the Media Foundation MP3 encoder (Windows 8 and newer). */
    bool isAvailable();

    /** Starts an MP3 file (16-bit PCM in). nullptr + error text on failure. */
    Writer* open (const wchar_t* path, int sampleRate, int numChannels, int kbps, std::string& error);

    /** Adds interleaved 16-bit samples. */
    bool write (Writer*, const int16_t* interleaved, int numFrames, std::string& error);

    /** Finishes the file and frees the writer (always, even when it fails). */
    bool close (Writer*, std::string& error);
}

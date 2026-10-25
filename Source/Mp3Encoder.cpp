#include "Mp3Encoder.h"

#if JUCE_WINDOWS
 #include "Mp3WriterWin.h"
#endif

namespace mp3
{

bool isAvailable()
{
   #if JUCE_WINDOWS
    return mfmp3::isAvailable();
   #else
    return false;
   #endif
}

juce::String encodeWavFile (const juce::File& wav, const juce::File& mp3File, int kbps, const std::atomic<bool>& cancel)
{
   #if JUCE_WINDOWS
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));

    if (reader == nullptr)
        return "Couldn't read the rendered audio";

    const int channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    const int rate = (int) reader->sampleRate;

    mp3File.deleteFile();
    std::string error;
    auto* writer = mfmp3::open (mp3File.getFullPathName().toWideCharPointer(), rate, channels, kbps, error);

    if (writer == nullptr)
        return "MP3 encoder: " + juce::String (error);

    constexpr int block = 8192;
    juce::AudioBuffer<float> buffer (channels, block);
    std::vector<int16_t> pcm ((size_t) (block * channels));
    bool ok = true;

    for (juce::int64 pos = 0; pos < reader->lengthInSamples && ok; pos += block)
    {
        if (cancel)
        {
            ok = false;
            error = "cancelled";
            break;
        }

        const int n = (int) juce::jmin ((juce::int64) block, reader->lengthInSamples - pos);
        reader->read (&buffer, 0, n, pos, true, channels > 1);

        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
                pcm[(size_t) (i * channels + c)] = (int16_t) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, buffer.getSample (c, i)) * 32767.0f);

        ok = mfmp3::write (writer, pcm.data(), n, error);
    }

    std::string closeError;
    const bool closed = mfmp3::close (writer, closeError);   // always frees the writer

    if (ok && ! closed)
        error = closeError;

    if (! ok || ! closed)
    {
        mp3File.deleteFile();
        return error == "cancelled" ? juce::String ("cancelled") : "MP3 encoder: " + juce::String (error);
    }

    return {};
   #else
    juce::ignoreUnused (wav, mp3File, kbps, cancel);
    return "MP3 export needs Windows";
   #endif
}

} // namespace mp3

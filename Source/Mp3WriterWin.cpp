// MP3 writing with Windows' Media Foundation (its built-in MP3 encoder and MP3 file sink,
// Windows 8 and newer). Kept apart from JUCE: see Mp3WriterWin.h.

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <vector>
#include <cstring>

#include "Mp3WriterWin.h"

namespace mfmp3
{

template <typename T>
static void release (T*& p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

static std::string hresultText (const char* what, HRESULT hr)
{
    char buffer[96];
    std::snprintf (buffer, sizeof (buffer), "%s failed (0x%08lx)", what, (unsigned long) hr);
    return buffer;
}

struct Writer
{
    bool comStarted = false, mfStarted = false;
    IMFSinkWriter* sink = nullptr;
    DWORD stream = 0;
    int rate = 44100, channels = 2;
    LONGLONG framesWritten = 0;

    ~Writer()
    {
        release (sink);

        if (mfStarted)
            MFShutdown();

        if (comStarted)
            CoUninitialize();
    }
};

static bool startUp (Writer& w, std::string& error)
{
    const HRESULT com = CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    if (SUCCEEDED (com))
        w.comStarted = true;          // (RPC_E_CHANGED_MODE: COM is already on for this thread; fine)
    else if (com != RPC_E_CHANGED_MODE)
    {
        error = hresultText ("Starting COM", com);
        return false;
    }

    const HRESULT hr = MFStartup (MF_VERSION, MFSTARTUP_LITE);

    if (FAILED (hr))
    {
        error = hresultText ("Starting Media Foundation", hr);
        return false;
    }

    w.mfStarted = true;
    return true;
}

bool isAvailable()
{
    // Is there an MP3 encoder registered?
    Writer w;
    std::string error;

    if (! startUp (w, error))
        return false;

    MFT_REGISTER_TYPE_INFO output { MFMediaType_Audio, MFAudioFormat_MP3 };
    IMFActivate** found = nullptr;
    UINT32 count = 0;

    if (FAILED (MFTEnumEx (MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_ALL, nullptr, &output, &found, &count)))
        return false;

    for (UINT32 i = 0; i < count; ++i)
        found[i]->Release();

    CoTaskMemFree (found);
    return count > 0;
}

Writer* open (const wchar_t* path, int sampleRate, int numChannels, int kbps, std::string& error)
{
    auto* w = new Writer();
    w->rate = sampleRate;
    w->channels = numChannels;

    IMFAttributes* attributes = nullptr;
    IMFMediaType* out = nullptr;
    IMFMediaType* in = nullptr;
    HRESULT hr = S_OK;

    auto fail = [&] (const char* what) -> Writer*
    {
        error = hresultText (what, hr);
        release (attributes);
        release (out);
        release (in);
        delete w;
        return nullptr;
    };

    if (! startUp (*w, error))
    {
        delete w;
        return nullptr;
    }

    if (FAILED (hr = MFCreateAttributes (&attributes, 1)))                                         return fail ("MFCreateAttributes");
    if (FAILED (hr = attributes->SetGUID (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3))) return fail ("Choosing the MP3 file type");
    if (FAILED (hr = MFCreateSinkWriterFromURL (path, nullptr, attributes, &w->sink)))              return fail ("Creating the MP3 file");

    // What comes out: MP3 at a constant bit rate.
    if (FAILED (hr = MFCreateMediaType (&out)))                                                     return fail ("MFCreateMediaType");
    out->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_MP3);
    out->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
    out->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) numChannels);
    out->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (kbps * 1000 / 8));

    if (FAILED (hr = w->sink->AddStream (out, &w->stream)))                                         return fail ("Setting up the MP3 encoder");

    // What goes in: 16-bit PCM.
    if (FAILED (hr = MFCreateMediaType (&in)))                                                      return fail ("MFCreateMediaType");
    in->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
    in->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) numChannels);
    in->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) (numChannels * 2));
    in->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (sampleRate * numChannels * 2));
    in->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    if (FAILED (hr = w->sink->SetInputMediaType (w->stream, in, nullptr)))                          return fail ("Feeding the MP3 encoder");
    if (FAILED (hr = w->sink->BeginWriting()))                                                      return fail ("Starting the MP3 file");

    release (attributes);
    release (out);
    release (in);
    return w;
}

bool write (Writer* w, const int16_t* interleaved, int numFrames, std::string& error)
{
    if (w == nullptr || w->sink == nullptr || numFrames <= 0)
        return w != nullptr;

    const DWORD bytes = (DWORD) (numFrames * w->channels * 2);
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    BYTE* data = nullptr;
    HRESULT hr = MFCreateMemoryBuffer (bytes, &buffer);

    if (SUCCEEDED (hr))
        hr = buffer->Lock (&data, nullptr, nullptr);

    if (SUCCEEDED (hr))
    {
        std::memcpy (data, interleaved, bytes);
        buffer->Unlock();
        hr = buffer->SetCurrentLength (bytes);
    }

    if (SUCCEEDED (hr))
        hr = MFCreateSample (&sample);

    if (SUCCEEDED (hr))
        hr = sample->AddBuffer (buffer);

    // Times in 100-nanosecond units.
    if (SUCCEEDED (hr))
        hr = sample->SetSampleTime (w->framesWritten * 10000000LL / w->rate);

    if (SUCCEEDED (hr))
        hr = sample->SetSampleDuration ((LONGLONG) numFrames * 10000000LL / w->rate);

    if (SUCCEEDED (hr))
        hr = w->sink->WriteSample (w->stream, sample);

    release (sample);
    release (buffer);

    if (FAILED (hr))
    {
        error = hresultText ("Encoding MP3", hr);
        return false;
    }

    w->framesWritten += numFrames;
    return true;
}

bool close (Writer* w, std::string& error)
{
    if (w == nullptr)
        return false;

    bool ok = true;

    if (w->sink != nullptr)
    {
        const HRESULT hr = w->sink->Finalize();

        if (FAILED (hr))
        {
            error = hresultText ("Finishing the MP3 file", hr);
            ok = false;
        }
    }

    delete w;
    return ok;
}

} // namespace mfmp3

#pragma once

#include "SongModel.h"

class PatternCompiler;
class MixerRouter;

/**
    Exports the song (or one pattern) to an audio file, optionally with one file per
    mixer insert ("stems").

    It works on a private copy of the song made at the moment you press Export, so
    whatever you or the others change meanwhile doesn't end up half-way in the file,
    and nothing in the shared song is touched (stems are made by muting tracks in
    the copy). Rendering runs in the background, one file at a time; MP3 is
    encoded afterwards with Windows' own MP3 encoder.

    Uses this PC's plugins and audio files: a plugin that isn't installed here, or
    an audio file that hasn't arrived yet, is silent in the export.
*/
class Exporter  : private juce::Timer
{
public:
    enum class Format { wav24, wav16, flac, mp3_320, mp3_192 };

    struct Options
    {
        bool wholeSong = true;            // false: one pattern, as a seamless loop
        juce::String patternSid;
        Format format = Format::wav24;
        bool stems = false;               // also one file per mixer insert
        juce::File destination;           // the mix; stems go in "<name> - stems" next to it
    };

    struct Result
    {
        juce::String error;
        juce::Array<juce::File> files;
        int missingAudio = 0, missingPlugins = 0;
        bool cancelled = false;
        bool ok() const noexcept          { return error.isEmpty() && ! cancelled; }
    };

    Exporter (te::Engine&, AssetStore&);
    ~Exporter() override;

    /** Starts an export of this song state (from EditSync::createSnapshot).
        Returns an error text, or "" if it started. onFinished runs on the message thread. */
    juce::String start (const juce::ValueTree& songState, const Options&, std::function<void (const Result&)> onFinished);
    void cancel();
    bool isRunning() const noexcept       { return running; }
    /** 0 to 1. */
    double getProgress() const noexcept   { return progress; }
    juce::String getStatus() const        { return status; }

    static bool canExportMp3();
    static juce::String extensionFor (Format);
    static juce::String describe (Format);

private:
    te::Engine& engine;
    AssetStore& assets;
    std::unique_ptr<te::Edit> copy;
    std::unique_ptr<PatternCompiler> compiler;
    std::unique_ptr<MixerRouter> router;
    std::unique_ptr<te::RenderQueue> queue;
    juce::ThreadPool encoderPool { 1 };

    struct Planned { juce::File renderFile, finalFile; };
    std::vector<Planned> planned;
    Options options;
    Result result;
    std::function<void (const Result&)> onFinished;
    bool running = false;
    std::atomic<bool> cancelEncoding { false };
    std::atomic<double> progress { 0.0 };
    std::atomic<int> encodedSoFar { 0 };
    juce::String status;

    void rendersFinished();
    void encodeMp3s();
    void finish();
    void cleanUp();
    void timerCallback() override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Exporter)
    JUCE_DECLARE_NON_COPYABLE (Exporter)
};

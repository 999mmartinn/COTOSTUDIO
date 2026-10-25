#include "SamplePreviewer.h"

SamplePreviewer::SamplePreviewer (te::Engine& e) : engine (e)
{
    formats.registerBasicFormats();
    readAhead.startThread (juce::Thread::Priority::normal);
    player.setSource (&transport);
    player.setGain (gain);
    engine.getDeviceManager().deviceManager.addAudioCallback (&player);
}

SamplePreviewer::~SamplePreviewer()
{
    engine.getDeviceManager().deviceManager.removeAudioCallback (&player);
    player.setSource (nullptr);
    transport.setSource (nullptr);
    reader.reset();
    readAhead.stopThread (2000);
}

bool SamplePreviewer::play (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));

    if (r == nullptr || r->sampleRate <= 0.0 || r->lengthInSamples <= 0)
        return false;

    const double rate = r->sampleRate;
    auto fresh = std::make_unique<juce::AudioFormatReaderSource> (r.release(), true);

    transport.stop();
    transport.setSource (fresh.get(), 32768, &readAhead, rate, 2);   // mono files play on both sides
    reader = std::move (fresh);
    file = f;
    transport.setPosition (0.0);
    transport.start();
    sendChangeMessage();
    return true;
}

void SamplePreviewer::stop()
{
    transport.stop();
    sendChangeMessage();
}

bool SamplePreviewer::isPlaying() const
{
    return transport.isPlaying();
}

double SamplePreviewer::getLengthSeconds() const
{
    return reader != nullptr ? transport.getLengthInSeconds() : 0.0;
}

double SamplePreviewer::getPositionSeconds() const
{
    return reader != nullptr ? transport.getCurrentPosition() : 0.0;
}

void SamplePreviewer::setGain (float g)
{
    gain = juce::jlimit (0.0f, 1.0f, g);
    player.setGain (gain);
}

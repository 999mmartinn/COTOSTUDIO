#include "MixerRouter.h"

MixerRouter::MixerRouter (te::Edit& e) : edit (e)
{
    edit.state.addListener (this);
    applyNow();
    startTimer (500);
}

MixerRouter::~MixerRouter()
{
    stopTimer();
    cancelPendingUpdate();
    edit.state.removeListener (this);
}

void MixerRouter::applyNow()
{
    cancelPendingUpdate();
    const juce::ScopedValueSetter<bool> svs (applying, true);

    for (auto* track : te::getAudioTracks (edit))
    {
        auto* wanted = song::resolveRoute (edit, *track);
        auto& output = track->getOutput();

        if (wanted != nullptr)
        {
            if (output.getDestinationTrack() != wanted)
                output.setOutputToTrack (wanted);
        }
        else if (output.getDestinationTrack() != nullptr || ! output.usesDefaultAudioOut())
        {
            output.setOutputToDefaultDevice (false);
        }
    }
}

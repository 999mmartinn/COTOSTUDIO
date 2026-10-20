#pragma once

#include <JuceHeader.h>

namespace te = tracktion;

// Small helpers shared by the UI and the self-test.
namespace song
{
    constexpr int numSteps = 16;               // one bar of 16th notes
    constexpr double stepBeats = 0.25;

    inline te::AudioTrack* getTrack (te::Edit& edit, int index)
    {
        auto tracks = te::getAudioTracks (edit);
        return juce::isPositiveAndBelow (index, tracks.size()) ? tracks[index] : nullptr;
    }

    inline te::MidiClip* findPatternClip (te::Edit& edit, int trackIndex)
    {
        if (auto* t = getTrack (edit, trackIndex))
            for (auto* c : t->getClips())
                if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                    return mc;

        return nullptr;
    }

    inline te::MidiNote* findNote (te::MidiClip& clip, int pitch, int step)
    {
        for (auto* n : clip.getSequence().getNotes())
            if (n->getNoteNumber() == pitch
                 && juce::roundToInt (n->getStartBeat().inBeats() / stepBeats) == step)
                return n;

        return nullptr;
    }

    inline bool hasNote (te::Edit& edit, int trackIndex, int pitch, int step)
    {
        if (auto* clip = findPatternClip (edit, trackIndex))
            return findNote (*clip, pitch, step) != nullptr;

        return false;
    }

    inline void setNote (te::Edit& edit, int trackIndex, int pitch, int step, bool on)
    {
        auto* clip = findPatternClip (edit, trackIndex);

        if (clip == nullptr)
            return;

        auto* existing = findNote (*clip, pitch, step);

        if (on && existing == nullptr)
            clip->getSequence().addNote (pitch, te::BeatPosition::fromBeats (step * stepBeats),
                                         te::BeatDuration::fromBeats (stepBeats), 100, 0,
                                         &edit.getUndoManager());
        else if (! on && existing != nullptr)
            clip->getSequence().removeNote (*existing, &edit.getUndoManager());
    }

    /** Adds a track with the built-in synth and an empty 1-bar pattern. */
    inline te::AudioTrack* addSynthTrack (te::Edit& edit)
    {
        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, te::getAudioTracks (edit).getLast()), nullptr);

        if (track == nullptr)
            return nullptr;

        track->setName ("Track " + juce::String (te::getAudioTracks (edit).size()));

        if (auto synth = edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}))
            track->pluginList.insertPlugin (synth, 0, nullptr);

        track->insertMIDIClip ("Pattern", edit.tempoSequence.toTime ({ te::BeatPosition::fromBeats (0.0),
                                                                       te::BeatPosition::fromBeats (4.0) }), nullptr);
        return track.get();
    }

    /** Inserts a plugin before the track's volume/meter plugins. */
    inline te::Plugin::Ptr insertPlugin (te::AudioTrack& track, const juce::PluginDescription& desc)
    {
        auto plugin = track.edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc);

        if (plugin == nullptr)
            return {};

        int index = 0;
        auto plugins = track.pluginList.getPlugins();

        for (; index < plugins.size(); ++index)
            if (dynamic_cast<te::VolumeAndPanPlugin*> (plugins[index].get()) != nullptr
                 || dynamic_cast<te::LevelMeterPlugin*> (plugins[index].get()) != nullptr)
                break;

        track.pluginList.insertPlugin (plugin, index, nullptr);
        return plugin;
    }

    inline te::ExternalPlugin* findExternalPlugin (te::Edit& edit, const juce::String& name)
    {
        for (auto* p : te::getAllPlugins (edit, false))
            if (auto* ext = dynamic_cast<te::ExternalPlugin*> (p))
                if (name.isEmpty() || ext->getName() == name)
                    return ext;

        return nullptr;
    }
}

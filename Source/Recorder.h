#pragma once

#include <JuceHeader.h>
#include <map>
#include "AssetStore.h"

namespace te = tracktion;

/**
    Records this PC's audio inputs (interface + mic) into the song, FL-style:
    a mixer insert gets an input and is armed; pressing Record records every
    armed insert's input.

      - Which input feeds which insert, and whether it is armed, belongs to this
        PC only (your mic is yours). It lives in the engine's INPUTDEVICES tree,
        which is never sent, and is re-applied if the song object is rebuilt.
      - An assigned input is heard through its insert's effects (monitoring),
        so you can sing through autotune / EQ while recording.
      - When a take ends it is converted (in the background) into a shared
        audio file (AssetStore) and added to the song as a new audio channel
        routed to that insert, with a playlist clip at exactly the recorded
        position. From there it travels to everyone like an imported file.
*/
class Recorder  : private te::TransportControl::Listener
{
public:
    Recorder (te::Engine&, AssetStore&);
    ~Recorder() override;

    /** Called by the Session whenever its Edit is created or replaced (nullptr = gone). */
    void attachTo (te::Edit*);

    struct Input { juce::String id, name; };
    /** This PC's audio inputs (one per interface input, mono). */
    juce::Array<Input> getInputs() const;

    /** The input feeding an insert ("" = none). */
    juce::String getInputFor (const juce::String& insertID) const;
    /** Sets (or clears with "") the input of an insert; a new input is armed right away. */
    void setInputFor (const juce::String& insertID, const juce::String& inputID);
    bool isArmed (const juce::String& insertID) const;
    void setArmed (const juce::String& insertID, bool);
    int getNumArmed() const;

    /** Starts recording every armed insert from this position. False if nothing is armed. */
    bool start (double startSeconds);
    bool isRecording() const;
    /** True while takes are still being turned into shared files. */
    bool isBusy() const                                  { return pendingTakes > 0; }

    std::function<void (const juce::String&)> onLog;
    /** Called on the message thread when a take has been added to the song. */
    std::function<void (const juce::ValueTree& clip)> onTakeAdded;

    /** Where the engine writes a new raw take for this Edit (see AppEngineBehaviour). */
    static juce::File newRecordingFile (te::Edit&, const juce::String& extension);

private:
    te::Engine& engine;
    AssetStore& assets;
    te::Edit* edit = nullptr;

    struct Choice { juce::String inputID; bool armed = false; };
    std::map<juce::String, Choice> choices;   // insert ID -> this PC's choice
    std::atomic<int> pendingTakes { 0 };
    juce::ThreadPool pool { 1 };

    te::InputDeviceInstance* findInstance (const juce::String& inputID) const;
    void applyChoices();
    void log (const juce::String& s)                     { if (onLog) onLog (s); }

    void recordingFinished (te::InputDeviceInstance&, te::EditItemID targetID,
                            const juce::ReferenceCountedArray<te::Clip>& recordedClips) override;
    void addTake (const AssetStore::ImportResult&, const juce::String& insertID,
                  double startSeconds, double offsetSeconds, double durationSeconds);

    JUCE_DECLARE_WEAK_REFERENCEABLE (Recorder)
};

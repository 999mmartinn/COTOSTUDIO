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
class Recorder  : private te::TransportControl::Listener,
                  private juce::Timer
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
    /** Inserts that have an input but aren't armed (they could be armed with one click). */
    juce::StringArray getDisarmedWithInput() const;
    /** Disarms every insert (inputs stay assigned; you still hear them). */
    void disarmAll();
    /** Arms every insert that has an input. */
    void armAllWithInput();

    /** Whether assigned inputs are heard through their inserts (monitoring). Turn it
        off if your interface's own "direct monitor" already lets you hear yourself. */
    void setMonitoring (bool);
    bool isMonitoring() const noexcept                   { return monitoring; }

    /** Monitoring of one insert's input (the playlist track's speaker button, 1.12): heard
        only while this and the overall monitoring are both on. On by default. */
    bool isMonitored (const juce::String& insertID) const;
    void setMonitored (const juce::String& insertID, bool);

    /** Which input feeds which insert, as text (kept in your settings and in templates).
        Inserts are matched by ID and name, so another song's inserts are never picked up.
        onlyThisSong: just the current song's inserts (for a template). */
    juce::String saveChoices (bool onlyThisSong = false) const;
    /** Adds the choices saved by saveChoices() for inserts of the current song. */
    void restoreChoices (const juce::String&);

    /** Punch in / out (like FL): only the part of a take between punch-in and
        punch-out is kept, and recording stops by itself at the punch-out while the
        song plays on. Playback starts `preRollBars` before the punch-in.
        Beats are song beats; this PC only. */
    struct Punch
    {
        bool on = false;
        double inBeat = 0.0, outBeat = 0.0;
        int preRollBars = 1;

        bool hasRange() const noexcept                   { return outBeat > inBeat + 0.01; }
        bool isActive() const noexcept                   { return on && hasRange(); }
    };

    void setPunch (const Punch& p)                       { punch = p; punch.preRollBars = juce::jlimit (0, 4, punch.preRollBars); }
    const Punch& getPunch() const noexcept               { return punch; }

    /** Starts recording every armed insert from this position (with punch in/out on:
        from the pre-roll before the punch-in instead). False if nothing is armed. */
    bool start (double startSeconds);

    /** Bars of metronome before the take starts (0, 1 or 2). The take still starts
        exactly at the start position. */
    void setCountInBars (int bars)                       { countInBars = juce::jlimit (0, 2, bars); }
    int getCountInBars() const noexcept                  { return countInBars; }
    /** Where the playhead starts when recording (before the start position with a count-in). */
    double getPreRollStart (double startSeconds) const;
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

    struct Choice
    {
        juce::String inputID;
        bool armed = false;
        juce::String insertName;   // to tell this song's insert from another song's with the same ID
        bool live = false;         // matched to an insert of the current song
        bool monitored = true;     // heard through the insert (with the overall monitoring on)
    };
    /** The monitor mode an input should have: on if any insert it feeds is monitored. */
    bool inputMonitored (const juce::String& inputID) const;
    void enableInput (const juce::String& inputID);
    /** The choice for an insert of the current song, or nullptr. */
    const Choice* liveChoice (const juce::String& insertID) const;
    int countInBars = 0;
    bool monitoring = true;
    Punch punch;
    double takePunchIn = -1.0, takePunchOut = -1.0;   // seconds, for the take being recorded (-1 = no punch)
    void timerCallback() override;
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

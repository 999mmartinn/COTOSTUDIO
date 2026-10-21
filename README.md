# SyncTest — a multiplayer, FL-style DAW for Windows (work in progress)

Several people work on the **same song at the same time**, each on their own PC,
each using their own native **VST3 plugins** (Waves, FabFilter...).

Built step by step, with stability first. Current step:

**Step 1 – Channel rack + patterns (v0.2)**
* **Channel rack**: one row per instrument with mute, volume and pan knobs and a
  step sequencer. Click or drag across steps to paint them.
* **Built-in drum synth** (Kick, Clap, Hat, Snare – pure synthesis, no sample
  files) and the built-in **4OSC** synth. **+ Channel** also lists your own VST3
  instruments.
* **Patterns**: several patterns (1, 2 or 4 bars), **+** for a new one,
  **...** to rename / clone / delete. Pattern mode: Play loops the pattern you
  are viewing; switching pattern while playing switches it for everyone.
* **Piano roll** for the selected channel: click to add a note, drag to move or
  lengthen it, drag its right edge to resize, right-click to delete.
* **Effects per channel**: right-click a channel name → *Add effect* (Waves,
  FabFilter...), *Effects* → open / remove. Also *Open instrument*, rename, delete.
* **Tempo (BPM)**, **Undo/Redo** (Ctrl+Z / Ctrl+Y, your own changes),
  **Space** = play/stop.
* Everything above is shared live with everyone in the session.

Built on [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) (GPLv3)
and [JUCE](https://juce.com) (AGPLv3). This project is published under the
**GNU AGPLv3** (see `LICENSE`).

---

## How it works (short version)

* Every PC has its **own full copy** of the song and makes its **own audio**.
  Only *edits* travel over the network, never audio. That keeps it light and
  makes latency only matter for "when do I see your change", not for playback.
* One PC **hosts**, the others **join**. The host passes every change on to
  everyone else.
* Two kinds of changes are sent:
  1. **Song edits** – channels, patterns, notes, plugin inserts, built-in synth
     settings, tempo, volume/pan/mute.
     Every changed part is sent as "this is how it looks now"; the other PC
     compares and changes only what differs. Notes get a small sync ID so two
     people can edit the same pattern at the same time without erasing each
     other's notes.
  2. **Plugin knobs** – when you turn a knob in a Waves/FabFilter window, the
     new value is sent (about 30 times a second while dragging).
     When you **close** a plugin window, its full state (preset) is sent too.
* Every 1.5 s the host sends a fingerprint of the song. If a joiner's copy
  ever differs, it automatically asks for a fresh copy.
* **Plugins are per person.** The song only says "FabFilter Pro-Q 3 with these
  settings". Each PC must have that plugin installed and licensed. If you
  don't have it, the slot shows *(not installed here)* and the song still
  works; you just don't hear that plugin.

## Running the test with a friend

1. Both of you: download `SyncTest.exe`, run it.
2. Both of you, the first time: press **Scan plugins...** → in that window use
   **Options → Scan for new or updated VST3 plug-ins** (default folder
   `C:\Program Files\Common Files\VST3` is where Waves and FabFilter install).
   *Waves:* make sure Waves Central installed the VST3 "WaveShell" files.
3. Host: press **Host** (port 47800 by default).
4. Friend: type the host's address in **Host address**, press **Join**.
   The status turns green: "Joined as peer 1 - in sync".
5. Try it:
   * click steps in the channel rack or notes in the piano roll on either PC –
     the other sees them;
   * right-click a channel name → **Add effect** → pick a Waves/FabFilter
     plugin – it appears on both PCs;
   * click the plugin's button to open its window and move a knob – the
     other PC's knob moves;
   * press **Play** on either PC – both play the loop.

### Which address do I type?

* **Same house / same Wi-Fi:** the host's local IP (in Windows: `ipconfig`,
  "IPv4 Address", e.g. `192.168.1.23`).
* **Over the internet**, choose one:
  * **A free virtual LAN app** (easiest, no router changes): both install
    Tailscale (recommended) or Radmin VPN, join the same network (in Tailscale:
    share the host's machine with the friend's account), and type the host's
    address from that app (Tailscale addresses start with `100.`).
  * **Port forwarding:** the host forwards **TCP port 47800** on the router to
    their PC and gives the friend their public IP.
* Windows Firewall will ask the host to allow SyncTest the first time:
  allow it on private networks.

You can also run **two copies on one PC** to try it alone: Host in one,
Join `127.0.0.1` in the other.

## Known limits of this test (on purpose)

* If two people change the *same thing* at the same moment (both move the
  same knob, or reorder tracks), the last change wins. Rarely the copies can
  briefly differ; the fingerprint check catches it and the joiner gets a fresh
  copy within a few seconds.
* A knob turned in the first half-second after a plugin is inserted may not be
  sent (the plugin is still loading). Closing its window sends its full state.
* Plugin *audio* isn't compared, only settings. A plugin that behaves
  differently on two PCs (different version) will sound different.
* What the engine plays is generated on each PC from the shared patterns
  (never sent), so every PC plays exactly the same notes.
* Not yet: playlist/arrangement, mixer view, recording/audio clips, saving
  projects, chat. Undo only undoes your own changes.
* No encryption or passwords: only join people you trust, on a private network.

## Self-test

`SyncTest.exe --selftest --plugin=<path to SyncTestGain.vst3>` runs a host
and two joiners inside one process over real TCP and checks notes, built-in
plugin settings, third-party plugin insertion and knob sync (both directions,
including a fast 200-step drag), simultaneous edits, ID clashes, Play/Stop,
leaving and rejoining. The Windows build on GitHub runs this automatically.

## Building

Needs CMake ≥ 3.22 and Visual Studio 2022 (Desktop C++ workload).

```
git clone --recurse-submodules <this repo>
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`external/tracktion_engine` is a git submodule (it brings JUCE with it).

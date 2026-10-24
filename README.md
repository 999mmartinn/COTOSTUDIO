# SyncTest — a multiplayer, FL-style DAW for Windows (work in progress)

Several people work on the **same song at the same time**, each on their own PC,
each using their own native **VST3 plugins** (Waves, FabFilter...).

Built step by step, with stability first.

**Step 4a – Audio clips + file sharing (v0.5)**
* **Drop audio files** (WAV, MP3, FLAC, AIFF, OGG) from Explorer onto a playlist
  lane, or press **Import audio...**. Each file gets its own **audio channel**
  (in the channel rack, routable to a mixer insert like any channel) and a clip
  in the playlist showing its waveform. Audio clips play in **SONG** mode.
* Drag an audio clip to move it; drag its **left or right edge to trim** it.
  Right-click deletes it. Clicking it selects its channel. Trimming is in
  seconds, so changing the tempo never stretches your audio.
* **The file is sent to everyone automatically** in the background (as FLAC,
  lossless and about half the size of WAV). Until it arrives, others see
  "receiving NN%" or "waiting for the file..." on the clip; it starts playing
  as soon as it's there. Song edits keep flowing while a big file transfers.
* Every file is identified by a fingerprint of its content, and checked when
  it arrives, so a broken transfer is never used (it's simply asked for again).
  Someone who joins later receives all the song's files.
* Files are kept in `%APPDATA%\SyncTest\Assets` on each PC.
* Copies from v0.4 and older can't join a v0.5 host (and vice versa): everyone
  needs the same version.

**Step 3 – Mixer (v0.4)**
* Bottom panel has two tabs: **Piano roll (F7)** and **Mixer (F9)**.
* **Master** strip plus **inserts** (+ Insert): volume fader, dB readout, pan,
  on/mute, level meter, and where the insert sends its sound (Master or another
  insert; loops are prevented). Right-click an insert's name to rename or delete it.
* **Effect slots** (10 per strip, master included): click a strip, then click an
  empty slot to add an effect – your VST3s (Waves, FabFilter…) or built-ins
  (EQ, compressor, reverb, delay, chorus, phaser, filter). Click a filled slot to
  open it; right-click to bypass, move up/down, replace or remove.
* **Routing channels**: the small box on each channel-rack row shows its insert
  (M = master). Click it to pick an insert or make a **new insert** named after
  the channel (like FL's "route to free insert").
* Everything syncs except the level meters (each PC measures its own audio).

**Step 2 – Playlist (v0.3)**
* **PAT / SONG** buttons next to Play (like FL). PAT loops the pattern you are
  editing; SONG plays the arrangement. The mode is shared with everyone.
* **Playlist** on top: lanes ("Track 1-16") where patterns are placed as clips.
  Click to place the selected pattern, drag to move it, drag its right edge to
  shorten or lengthen it, right-click to delete it, click a clip to edit its
  pattern below. **Snap** to bar / beat / step; **Ctrl + mouse wheel** zooms.
  Its ruler has its own green start marker for SONG mode.
* The playlist, channel rack and piano roll are all visible; drag the bars
  between them to resize.
* Deleting a pattern also removes its clips from the playlist.

**Step 1 – Channel rack + patterns (v0.2)**
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
     When you **close** a plugin window, its full state (preset) is sent too;
     that's the moment a preset you loaded reaches the others.
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
* Routing is stored by each insert's permanent ID and applied on every PC, so
  adding or deleting tracks never sends audio to the wrong insert.
* Audio files are sent person to person through the host. A joiner's file goes
  to the host first, then to the others, so the host's upload speed matters for
  big files (a 3-minute stereo recording is roughly 15-30 MB as FLAC).
* Not yet: recording (next step), solo and send knobs, saving projects, chat.
  Undo only undoes your own changes.
* No encryption or passwords: only join people you trust, on a private network.

## Self-test

`SyncTest.exe --selftest --plugin=<path to SyncTestGain.vst3>` runs a host
and two joiners inside one process over real TCP and checks notes, built-in
plugin settings, third-party plugin insertion and knob sync (both directions,
including a fast 200-step drag), simultaneous edits, ID clashes, Play/Stop,
the playlist, the mixer, audio file sharing (each simulated PC has its own
folder; includes a 10 MB file sent at a capped speed while edits keep flowing, a damaged transfer and a late joiner with no
files), leaving and rejoining. The Windows build on GitHub runs this automatically.

## Building

Needs CMake ≥ 3.22 and Visual Studio 2022 (Desktop C++ workload).

```
git clone --recurse-submodules <this repo>
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`external/tracktion_engine` is a git submodule (it brings JUCE with it).

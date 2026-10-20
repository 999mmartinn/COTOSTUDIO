# SyncTest — two-peer sync test for a multiplayer DAW

This is the first proof-of-concept for a multiplayer, pattern-based DAW for Windows.
It answers one question before we build anything bigger:

> Can two (or more) PCs edit the **same song at the same time**, using their own
> **native VST3 plugins** (Waves, FabFilter...), and stay in sync?

It is deliberately small: one window, tracks, a 16-step pattern grid, plugin
inserts, Play/Stop, Host/Join. Everything else comes later.

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
  1. **Song edits** – tracks, notes, plugin inserts, built-in synth settings.
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
   * click squares in the pattern grid on either PC – the other sees them;
   * **+ Plugin** → pick a Waves/FabFilter plugin – it appears on both PCs;
   * click the plugin's button to open its window and move a knob – the
     other PC's knob moves;
   * press **Play** on either PC – both play the loop.

### Which address do I type?

* **Same house / same Wi-Fi:** the host's local IP (in Windows: `ipconfig`,
  "IPv4 Address", e.g. `192.168.1.23`).
* **Over the internet**, choose one:
  * **A free virtual LAN app** (easiest, no router changes): both install the
    same one (e.g. Tailscale, ZeroTier or Radmin VPN), join the same network,
    and type the host's address from that app.
  * **Port forwarding:** the host forwards **TCP port 47800** on the router to
    their PC and gives the friend their public IP.
* Windows Firewall will ask the host to allow SyncTest the first time:
  allow it on private networks.

You can also run **two copies on one PC** to try it alone: Host in one,
Join `127.0.0.1` in the other.

## Known limits of this test (on purpose)

* Changes are addressed by position in the song. If two people change the
  *same* list at the exact same moment, the copies can briefly differ; the
  fingerprint check catches it and the joiner gets a fresh copy within a few
  seconds. The real product will use per-item IDs/CRDTs to avoid that.
* Plugin *audio* isn't compared, only settings. A plugin that behaves
  differently on two PCs (different version) will sound different.
* No recording, audio clips, mixer view, undo sync or chat yet.
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

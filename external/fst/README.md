# FST (Free Studio Technology)

VST2 headers used to host VST2 plugins (and to build the self-test's VST2 plugin).
FST is a reverse-engineered, free replacement for Steinberg's discontinued VST2 SDK,
written without the SDK: https://git.iem.at/zmoelnig/FST (copy taken from the
GitHub mirror https://github.com/pierreguillot/FTS, commit b16f9b7, FST 0.115.0).

Licence: GNU General Public License, version 3 or later (see LICENSE), which can be
combined with COTOSTUDIO's GNU AGPLv3.

The files sit in `pluginterfaces/vst2.x/`, the path JUCE includes the VST2 headers from.
Local changes in `fst.h`, each marked "COTOSTUDIO change": the effect struct is named
`AEffect` (to match JUCE's forward declaration); the event and speaker lists have the
fixed room hosts allocate (2 events, 8 speakers), and the pin properties have
the 48 spare bytes plugins expect; and a few opcode / constant values FST
0.115 left open are filled in (editor idle and top, idle, keys, bypass, tail size, programs,
MIDI key names, latency, the stereo / active pin flags, SMPTE rates), plus the names JUCE's VST2 host and plugin wrapper ask for; and two
fixes for Visual Studio (a 64-bit pointer-sized integer on Win64, and a missing macro).

VST is a trademark of Steinberg Media Technologies GmbH.

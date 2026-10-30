#include "FlpImport.h"
#include "SongModel.h"
#include "ParametricEQPlugin.h"
#include "LimiterPlugin.h"
#include "DelayPlugin.h"
#include "ReverbPlugin.h"
#include "SamplerPlugin.h"
#include "MoreFx.h"
#include <cmath>
#include <map>
#include <set>

namespace flimport
{

//==============================================================================
// Reading the file
namespace
{
    struct Event
    {
        int id = 0;
        const juce::uint8* data = nullptr;
        size_t size = 0;

        juce::uint32 u32() const    { return size >= 4 ? juce::ByteOrder::littleEndianInt (data) : (size >= 2 ? (juce::uint32) juce::ByteOrder::littleEndianShort (data) : (size >= 1 ? data[0] : 0u)); }
        int i32() const             { return (int) u32(); }

        /** FL's texts are UTF-16 (except the version, plain text). */
        juce::String text() const
        {
            if (size == 0)
                return {};

            bool utf16 = size >= 2 && size % 2 == 0;

            if (utf16)
            {
                int zeros = 0;

                for (size_t i = 1; i < size; i += 2)
                    zeros += data[i] == 0 ? 1 : 0;

                utf16 = zeros * 2 >= (int) (size / 2);   // mostly Latin characters: their high bytes are 0
            }

            if (utf16)
            {
                juce::String s;
                for (size_t i = 0; i + 1 < size; i += 2)
                {
                    const auto c = (juce::juce_wchar) (data[i] | (data[i + 1] << 8));
                    if (c == 0) break;
                    s += juce::String::charToString (c);
                }
                return s;
            }

            return juce::String::fromUTF8 ((const char*) data, (int) strnlen ((const char*) data, size));
        }
    };

    /** Splits the event stream. FL 26 writes event 172 with 3 bytes, so a variant is tried
        when the plain rule doesn't land exactly on the end. */
    bool readEvents (const juce::uint8* begin, const juce::uint8* end, std::vector<Event>& out, bool shortEvent172)
    {
        out.clear();
        auto* p = begin;

        while (p < end)
        {
            Event e;
            e.id = *p++;
            size_t n = 0;

            if (e.id == 172 && shortEvent172) n = 3;
            else if (e.id < 64)               n = 1;
            else if (e.id < 128)              n = 2;
            else if (e.id < 192)              n = 4;
            else
            {
                int shift = 0;

                for (;;)
                {
                    if (p >= end || shift > 28)
                        return false;

                    const auto b = *p++;
                    n |= (size_t) (b & 0x7f) << shift;
                    shift += 7;

                    if ((b & 0x80) == 0)
                        break;
                }
            }

            if ((size_t) (end - p) < n)
                return false;

            e.data = p;
            e.size = n;
            p += n;
            out.push_back (e);
        }

        return p == end;
    }

    juce::uint64 le64 (const juce::uint8* p)  { return juce::ByteOrder::littleEndianInt64 (p); }

    /** <u32 id><u64 size><bytes> records, from offset `start`. */
    std::map<int, juce::MemoryBlock> readRecords (const juce::uint8* data, size_t size, size_t start)
    {
        std::map<int, juce::MemoryBlock> records;
        size_t p = start;

        while (p + 12 <= size)
        {
            const auto id = (int) juce::ByteOrder::littleEndianInt (data + p);
            const auto n = le64 (data + p + 4);
            p += 12;

            if (n > size - p)
                break;

            records[id] = juce::MemoryBlock (data + p, (size_t) n);
            p += (size_t) n;
        }

        return records;
    }

    juce::String recordText (const std::map<int, juce::MemoryBlock>& r, int id)
    {
        auto it = r.find (id);
        return it != r.end() ? juce::String::fromUTF8 ((const char*) it->second.getData(), (int) strnlen ((const char*) it->second.getData(), it->second.getSize()))
                             : juce::String();
    }

    /** The plugin data of an FL "Fruity Wrapper" (a VST/VST3 plugin). */
    void readWrapper (const Event& data, Effect& fx)
    {
        fx.isVST = true;
        fx.missingInFL = true;   // until its plugin's name or file turns up

        if (data.size < 16)
            return;

        auto r = readRecords (data.data, data.size, 4);

        if (r.count (54) == 0 && r.count (55) == 0)
            return;   // FL couldn't load this plugin when it saved (only window data)

        fx.missingInFL = false;
        const auto kind = r.count (50) > 0 && r[50].getSize() >= 4 ? (int) juce::ByteOrder::littleEndianInt (r[50].getData()) : -1;
        fx.isVST3 = kind == 7 || kind == 8;   // effect / instrument
        fx.pluginName = recordText (r, 54);
        fx.file = recordText (r, 55);
        fx.vendor = recordText (r, 56);

        if (! fx.isVST3 && r.count (51) > 0 && r[51].getSize() >= 4)
            fx.vst2ID = (int) juce::ByteOrder::littleEndianInt (r[51].getData());

        if (fx.isVST3 && r.count (52) > 0 && r[52].getSize() == 16)
            fx.vst3ClassID = r[52];

        if (fx.isVST3 && r.count (53) > 0 && r[53].getSize() > 80)
        {
            auto& s = r[53];
            auto states = readRecords ((const juce::uint8*) s.getData(), s.getSize(), 80);

            if (states.count (3) > 0) fx.componentState = states[3];
            if (states.count (2) > 0) fx.controllerState = states[2];
        }
        else if (! fx.isVST3 && r.count (53) > 0 && r[53].getSize() > 21)
        {
            // A VST2: FL's small header, then the plugin's own chunk (often the same data its
            // VST3 version reads).
            auto* b = (const juce::uint8*) r[53].getData();
            const auto chunk = (size_t) juce::ByteOrder::littleEndianInt (b + 9);

            if ((int) juce::ByteOrder::littleEndianInt (b) == -9 && chunk > 0 && 21 + chunk <= r[53].getSize())
            {
                fx.componentState = juce::MemoryBlock (b + 21, chunk);
                fx.stateFromVST2 = true;
            }
        }
    }

    /** "VST3 plugin, named X inside its state": Waves keep it in an XML preset (<PluginName>). */
    juce::String nameInsideState (const juce::MemoryBlock& state)
    {
        const auto text = juce::String::fromUTF8 ((const char*) state.getData(), (int) juce::jmin ((size_t) 4096, state.getSize()));
        const auto tag = text.indexOf ("<PluginName>");

        if (tag < 0)
            return {};

        return text.substring (tag + 12).upToFirstOccurrenceOf ("<", false, false).trim();
    }
}

//==============================================================================
juce::String Effect::describe() const
{
    if (! isVST)
        return kind;

    if (pluginName.isEmpty())
        return displayName.isNotEmpty() ? displayName : kind;

    if (displayName.isNotEmpty() && ! pluginName.containsIgnoreCase ("WaveShell")
         && displayName.compareIgnoreCase (pluginName) != 0)
        return pluginName + " (\"" + displayName + "\")";

    return displayName.isNotEmpty() ? displayName : pluginName;
}

bool Insert::routesTo (int destination) const
{
    for (auto& r : routes)
        if (r.to == destination)
            return true;

    return false;
}

juce::String Insert::displayName() const
{
    if (name.isNotEmpty())
        return name;

    return number == 0 ? juce::String ("Master") : "Insert " + juce::String (number);
}

bool Project::isUsed (int n) const
{
    if (! juce::isPositiveAndBelow (n, inserts.size()) || n == 0)
        return false;

    auto& ins = inserts.getReference (n);
    const bool plainRouting = ins.routes.isEmpty() || (ins.routes.size() == 1 && ins.routes[0].to == 0);
    return ins.name.isNotEmpty() || ! ins.effects.isEmpty() || ! plainRouting || getChannelInserts().contains (n);
}

juce::Array<int> Project::getUsedInserts() const
{
    std::set<int> used;

    for (int n = 1; n < inserts.size(); ++n)
        if (isUsed (n))
            used.insert (n);

    // An unnamed, empty insert that a used one feeds is part of the setup too.
    for (bool grew = true; grew;)
    {
        grew = false;

        for (auto n : std::set<int> (used))
            for (auto& r : inserts.getReference (n).routes)
                if (r.to > 0 && used.insert (r.to).second)
                    grew = true;
    }

    juce::Array<int> result;
    for (auto n : used)
        result.add (n);
    return result;
}

const Channel* Project::findChannel (int index) const
{
    for (auto& c : channels)
        if (c.index == index)
            return &c;

    return nullptr;
}

juce::Array<int> Project::getChannelInserts() const
{
    juce::Array<int> result;

    for (auto& c : channels)
        if (c.insert > 0 && c.insert < inserts.size())
            result.addIfNotAlreadyThere (c.insert);

    result.sort();
    return result;
}

int Project::countSamples (bool foundOnly) const
{
    int n = 0;

    for (auto& c : channels)
        if (c.needsSample() && (! foundOnly || c.sampleFile.existsAsFile()))
            ++n;

    return n;
}

namespace
{
    /** The file name at the end of a path FL saved (with \ or /). */
    juce::String fileNameOf (const juce::String& flPath)
    {
        return flPath.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
    }

    /** Where FL's folder variables ("%FLStudioFactoryData%") can point on this PC. */
    juce::Array<juce::File> flFolderCandidates (const juce::String& variable)
    {
        juce::Array<juce::File> result;
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

        if (variable.equalsIgnoreCase ("FLStudioUserData"))
        {
            result.add (docs.getChildFile ("Image-Line").getChildFile ("FL Studio"));
            result.add (docs.getChildFile ("Image-Line").getChildFile ("Data").getChildFile ("FL Studio"));
        }

        if (variable.equalsIgnoreCase ("FLStudioFactoryData"))
        {
            // <program files>\Image-Line\FL Studio <version>\Data (every version installed)
            juce::Array<juce::File> programs { juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory) };
           #if JUCE_WINDOWS
            programs.addIfNotAlreadyThere (juce::File ("C:\\Program Files"));
            programs.addIfNotAlreadyThere (juce::File ("C:\\Program Files (x86)"));
           #endif

            for (auto& folder : programs)
                for (auto& fl : folder.getChildFile ("Image-Line").findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
                    result.addIfNotAlreadyThere (fl.getChildFile ("Data"));

            result.add (docs.getChildFile ("Image-Line").getChildFile ("Data"));
        }

        return result;
    }

    /** Files by (lower-case) name under a folder, a few levels deep. */
    void indexFolder (const juce::File& folder, std::map<juce::String, juce::File>& byName, int depth = 4)
    {
        if (! folder.isDirectory() || depth < 0)
            return;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto f = entry.getFile();

            if (entry.isDirectory())
                indexFolder (f, byName, depth - 1);
            else if (f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
                byName.emplace (f.getFileName().toLowerCase(), f);
        }
    }
}

void Project::findSamples (const juce::File& flpFile, const juce::Array<juce::File>& moreFolders)
{
    std::map<juce::String, juce::File> byName;
    bool indexed = false;

    for (auto& c : channels)
    {
        if (! c.needsSample() || c.sampleFile.existsAsFile())
            continue;

        // 1. Where FL saved it, its "%FLStudio...%" folder worked out.
        auto path = c.samplePath.replaceCharacter ('\\', '/');

        if (path.startsWithChar ('%'))
        {
            const auto variable = path.substring (1).upToFirstOccurrenceOf ("%", false, false);
            const auto rest = path.fromFirstOccurrenceOf ("%", false, false).fromFirstOccurrenceOf ("%", false, false).trimCharactersAtStart ("/");

            for (auto& base : flFolderCandidates (variable))
                if (auto f = base.getChildFile (rest); f.existsAsFile())
                {
                    c.sampleFile = f;
                    break;
                }
        }
        else if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
        {
            c.sampleFile = juce::File (path);
        }

        if (c.sampleFile.existsAsFile())
            continue;

        // 2. By name: next to the .flp (its Samples / Audio folders...) and the folders given.
        if (! indexed)
        {
            indexed = true;

            for (auto& folder : moreFolders)
                indexFolder (folder, byName);

            indexFolder (flpFile.getParentDirectory(), byName, 3);
        }

        if (auto it = byName.find (fileNameOf (c.samplePath).toLowerCase()); it != byName.end())
            c.sampleFile = it->second;
    }
}

double flLevelToDb (int value)
{
    if (value <= 0)
        return -100.0;

    // 12800 = 0 dB, 16000 (FL's 125 %) = +5.6 dB.
    return 5.6 * std::log ((double) value / 12800.0) / std::log (1.25);
}

juce::String parse (const juce::File& file, Project& project)
{
    juce::MemoryBlock data;

    if (! file.loadFileAsData (data))
        return "Could not read " + file.getFullPathName();

    return parse (data, project);
}

juce::String parse (const juce::MemoryBlock& file, Project& project)
{
    project = {};
    auto* d = (const juce::uint8*) file.getData();
    const auto size = file.getSize();

    if (size < 22 || memcmp (d, "FLhd", 4) != 0)
        return "This is not an FL Studio project (.flp)";

    const auto headerSize = (size_t) juce::ByteOrder::littleEndianInt (d + 4);
    size_t pos = 8 + headerSize;

    if (pos + 8 > size || memcmp (d + pos, "FLdt", 4) != 0)
        return "This FL Studio project is damaged (no data part)";

    const auto dataSize = (size_t) juce::ByteOrder::littleEndianInt (d + pos + 4);
    pos += 8;

    if (dataSize > size - pos)
        return "This FL Studio project is cut short";

    std::vector<Event> events;

    if (! readEvents (d + pos, d + pos + dataSize, events, false)
         && ! readEvents (d + pos, d + pos + dataSize, events, true))
        return "This FL Studio project could not be read (made with a newer FL Studio?)";

    //==============================================================================
    project.ppq = juce::jmax (1, (int) juce::ByteOrder::littleEndianShort (d + 12));
    const double ppq = project.ppq;

    std::map<int, Insert> inserts;
    std::map<int, juce::Array<bool>> routing;
    const Event* params = nullptr;

    // The beat: which channel / pattern the events belong to.
    enum class Context { none, channel, pattern };
    Context context = Context::none;
    int channelAt = -1;             // index in project.channels
    int envelopes = 0;              // envelope records of the current channel so far
    bool channelNamed = false;
    std::map<int, Pattern> patterns;
    int patternNumber = 0;
    const Event* playlist = nullptr;
    int arrangement = -1, wantedArrangement = -1;
    int current = 0;          // the mixer insert the events belong to (0 = master)
    int lastTrack = -1;       // playlist track of the last 238 event

    // The effect being read (201 ... 98); only mixer slots count (channels have plugins too)
    bool inEffect = false, inMixer = false;
    Effect fx;

    for (auto& e : events)
    {
        switch (e.id)
        {
            case 199: project.flVersion = e.text(); break;
            case 156: project.bpm = e.u32() / 1000.0; break;
            case 66:  if (project.bpm <= 0.0) project.bpm = (double) e.u32(); break;   // older FL versions
            case 17:  project.numerator = juce::jmax (1, (int) e.u32()); break;
            case 18:  project.denominator = juce::jmax (1, (int) e.u32()); break;

            case 238:   // playlist track data: u32 track number (1-based), u32 colour (R, G, B, 0), ...
                lastTrack = e.size >= 4 ? (int) e.u32() - 1 : -1;

                if (e.size >= 8 && juce::isPositiveAndBelow (lastTrack, 1000))
                {
                    const auto* rgb = e.data + 4;
                    const bool flDefault = rgb[0] == 0x48 && rgb[1] == 0x51 && rgb[2] == 0x56;   // FL's grey #485156

                    while (project.trackColours.size() <= lastTrack)
                        project.trackColours.add (juce::Colours::transparentBlack);

                    project.trackColours.set (lastTrack, flDefault ? juce::Colours::transparentBlack
                                                                   : juce::Colour (rgb[0], rgb[1], rgb[2]));
                }
                break;
            case 239:
                if (juce::isPositiveAndBelow (lastTrack, 1000))
                {
                    while (project.trackNames.size() <= lastTrack)
                        project.trackNames.add ({});

                    project.trackNames.set (lastTrack, e.text());
                }
                break;

            case 204: inserts[current].name = e.text(); break;

            case 201:   // a plugin starts (channels have them too; only mixer slots end with 98)
                fx = {};
                fx.kind = e.text();
                fx.isVST = fx.missingInFL = fx.kind == "Fruity Wrapper";   // its data says which plugin (event 213)
                inEffect = true;

                if (context == Context::channel && ! inMixer)
                    project.channels[(size_t) channelAt].kind = fx.kind;
                break;

            case 203:
                if (inEffect)
                    fx.displayName = e.text();

                if (context == Context::channel && ! inMixer && ! channelNamed)
                {
                    project.channels[(size_t) channelAt].name = e.text();
                    channelNamed = true;
                }
                break;

            case 213:
                if (inEffect && fx.kind != "Fruity Wrapper" && e.size > 0)
                    fx.flData.replaceAll (e.data, e.size);

                if (inEffect && fx.kind == "Fruity Wrapper")
                {
                    readWrapper (e, fx);

                    if (context == Context::channel && ! inMixer)
                        project.channels[(size_t) channelAt].plugin = fx;
                }
                break;

            case 236:   // an insert's header ends; its effect slots follow
                inMixer = true;
                inEffect = false;
                context = Context::none;
                break;

            //==============================================================================
            // The beat
            case 64:    // a channel starts
            {
                Channel c;
                c.index = (int) e.u32();
                project.channels.push_back (c);
                channelAt = (int) project.channels.size() - 1;
                context = Context::channel;
                envelopes = 0;
                channelNamed = false;
                inEffect = false;
                break;
            }

            case 21:  if (context == Context::channel) project.channels[(size_t) channelAt].type = (int) e.u32(); break;
            case 196: if (context == Context::channel) project.channels[(size_t) channelAt].samplePath = e.text(); break;
            case 104: if (context == Context::channel) project.channels[(size_t) channelAt].insert = (int) e.u32(); break;

            case 219:   // levels: i32 pan (0..12800, 6400 = centre), i32 volume (10000 = FL's default)
                if (context == Context::channel && e.size >= 8)
                {
                    auto& c = project.channels[(size_t) channelAt];
                    c.pan = juce::jlimit (-1.0, 1.0, ((int) juce::ByteOrder::littleEndianInt (e.data) - 6400) / 6400.0);
                    c.volume = juce::jmax (0.0, (int) juce::ByteOrder::littleEndianInt (e.data + 4) / 10000.0);
                }
                break;

            case 218:   // envelopes: panning, volume, mod X, mod Y, pitch; u32 #1 = -1 when on
                if (context == Context::channel)
                {
                    if (envelopes == 1 && e.size >= 8)
                        project.channels[(size_t) channelAt].volumeEnvelope = juce::ByteOrder::littleEndianInt (e.data + 4) == 0xffffffffu;

                    ++envelopes;
                }
                break;

            case 65:    // a pattern (its notes, or later its name)
                patternNumber = (int) e.u32();
                patterns[patternNumber].number = patternNumber;
                context = Context::pattern;
                break;

            case 193: if (context == Context::pattern) patterns[patternNumber].name = e.text(); break;

            case 224:   // a pattern's notes
                if (context == Context::pattern)
                {
                    auto& pat = patterns[patternNumber];

                    for (size_t i = 0; i + 24 <= e.size; i += 24)
                    {
                        auto* n = e.data + i;
                        Note note;
                        note.beat = juce::ByteOrder::littleEndianInt (n) / ppq;
                        note.channel = juce::ByteOrder::littleEndianShort (n + 6);
                        note.length = juce::ByteOrder::littleEndianInt (n + 8) / ppq;
                        note.key = juce::jlimit (0, 127, (int) juce::ByteOrder::littleEndianShort (n + 12));
                        note.fine = juce::jlimit (-100, 100, juce::roundToInt ((n[16] - 120) * 100.0 / 120.0));
                        note.release = juce::jlimit (0, 127, (int) n[18]);
                        note.pan = juce::jlimit (-100, 100, juce::roundToInt ((n[20] - 64) * 100.0 / 64.0));
                        note.velocity = juce::jlimit (1, 127, (int) n[21]);
                        pat.notes.push_back (note);
                    }
                }
                break;

            case 100: wantedArrangement = (int) e.u32(); break;   // the arrangement FL had open
            case 99:  arrangement = (int) e.u32(); context = Context::none; break;

            case 233:   // playlist items of the current arrangement (the open one, else the first)
                if (playlist == nullptr || arrangement == wantedArrangement)
                    playlist = &e;
                break;

            case 98:    // an effect slot of the current insert ends (empty or not)
                if (inEffect && inMixer)
                {
                    fx.slot = (int) e.u32();
                    inserts[current].effects.add (fx);
                }

                inEffect = false;
                break;

            case 235:   // the current insert's routing: one byte per destination
            {
                juce::Array<bool> r;
                for (size_t i = 0; i < e.size; ++i)
                    r.add (e.data[i] != 0);

                routing[current] = r;
                inserts[current].number = current;   // make sure it exists
                ++current;
                inEffect = false;
                break;
            }

            case 225: params = &e; break;
            default: break;
        }
    }

    if (routing.empty())
        return "No mixer found in this FL Studio project";

    //==============================================================================
    // The beat's patterns and playlist
    for (auto& [number, pat] : patterns)
        if (! pat.notes.empty())
            project.patterns.push_back (pat);

    if (playlist != nullptr)
    {
        const size_t itemSize = playlist->size % 88 == 0 ? 88 : playlist->size % 60 == 0 ? 60 : 32;

        for (size_t i = 0; i + itemSize <= playlist->size; i += itemSize)
        {
            auto* r = playlist->data + i;
            Item item;
            const auto base = (int) juce::ByteOrder::littleEndianShort (r + 4);
            const auto what = (int) juce::ByteOrder::littleEndianShort (r + 6);
            item.start = juce::ByteOrder::littleEndianInt (r) / ppq;
            item.length = juce::ByteOrder::littleEndianInt (r + 8) / ppq;
            item.track = 499 - (int) juce::ByteOrder::littleEndianShort (r + 12);

            if (base > 0 && what > base)
            {
                item.pattern = what - base;
                const auto startTicks = (int) juce::ByteOrder::littleEndianInt (r + 24);
                item.patternOffset = startTicks > 0 ? startTicks / ppq : 0.0;
            }
            else
            {
                item.channel = what;
                float startMs = 0.0f;
                std::memcpy (&startMs, r + 24, 4);   // (little-endian float, as on Windows)
                item.audioOffset = startMs > 0.0f ? startMs / 1000.0 : 0.0;
            }

            if (item.length > 0.0 && juce::isPositiveAndBelow (item.track, 500))
                project.items.push_back (item);
        }
    }

    //==============================================================================
    // Values from the mixer's parameter table
    std::map<std::pair<int, int>, int> slotOn, slotMix, insertValue, routeLevel;

    if (params != nullptr)
    {
        for (size_t i = 0; i + 12 <= params->size; i += 12)
        {
            auto* r = params->data + i;
            const int what = r[4], kind = r[5];
            const int code = juce::ByteOrder::littleEndianShort (r + 6);
            const int value = (int) juce::ByteOrder::littleEndianInt (r + 8);

            if (code < 0x7000)
                continue;

            const int ins = (code - 0x7000) >> 6, sub = (code - 0x7000) & 63;

            if (kind == 0x1f)
            {
                if (what == 0)       slotOn[{ ins, sub }] = value;
                else if (what == 1)  slotMix[{ ins, sub }] = value;
                else                 insertValue[{ ins, what }] = value;
            }
            else if (kind == 0x20)
            {
                routeLevel[{ ins, what }] = value;
            }
        }
    }

    const int count = routing.rbegin()->first + 1;

    for (int n = 0; n < count; ++n)
    {
        Insert ins = inserts.count (n) > 0 ? inserts[n] : Insert();
        ins.number = n;

        auto value = [&] (int what, int fallback)
        {
            auto it = insertValue.find ({ n, what });
            return it != insertValue.end() ? it->second : fallback;
        };

        ins.volumeDb = flLevelToDb (value (0xc0, 12800));
        ins.pan = juce::jlimit (-1.0, 1.0, value (0xc1, 0) / 6400.0);

        for (auto& f : ins.effects)
        {
            if (auto it = slotOn.find ({ n, f.slot }); it != slotOn.end())
                f.enabled = it->second != 0;

            if (auto it = slotMix.find ({ n, f.slot }); it != slotMix.end())
                f.mix = juce::jlimit (0.0, 1.0, it->second / 12800.0);
        }

        auto& r = routing[n];

        for (int to = 0; to < r.size(); ++to)
        {
            if (! r[to] || to == n)
                continue;

            Route route;
            route.to = to;
            auto it = routeLevel.find ({ n, to });
            const int level = it != routeLevel.end() ? it->second : 12800;
            route.closed = level <= 0;
            route.db = flLevelToDb (level);
            ins.routes.add (route);
        }

        project.inserts.add (ins);
    }

    return {};
}

//==============================================================================
// Finding the plugin on this PC
namespace
{
    juce::String normalise (const juce::String& s)
    {
        return s.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789");
    }

    /** JUCE's IDs for a VST3 class (see VST3PluginFormat): a hash of the class ID's bytes
        (deprecatedUid) and of its four 32-bit words, read the Windows (COM) way or not. */
    juce::Array<int> hashesForClass (const juce::MemoryBlock& cid)
    {
        juce::Array<int> result;

        if (cid.getSize() != 16)
            return result;

        auto* b = (const juce::uint8*) cid.getData();
        juce::uint32 bytesHash = 0;

        for (int i = 0; i < 16; ++i)
            bytesHash = bytesHash * 31 + (juce::uint32) (juce::int32) (juce::int8) b[i];

        result.add ((int) bytesHash);

        auto word = [b] (int i0, int i1, int i2, int i3)
        {
            return ((juce::uint32) b[i0] << 24) | ((juce::uint32) b[i1] << 16) | ((juce::uint32) b[i2] << 8) | (juce::uint32) b[i3];
        };

        const juce::uint32 com[4]   = { word (3, 2, 1, 0), word (5, 4, 7, 6), word (8, 9, 10, 11), word (12, 13, 14, 15) };
        const juce::uint32 plain[4] = { word (0, 1, 2, 3), word (4, 5, 6, 7), word (8, 9, 10, 11), word (12, 13, 14, 15) };

        for (auto* words : { com, plain })
        {
            juce::uint32 h = 0;
            for (int i = 0; i < 4; ++i)
                h = h * 31 + words[i];
            result.add ((int) h);
        }

        return result;
    }
}

const juce::PluginDescription* findPlugin (const juce::Array<juce::PluginDescription>& known, const Effect& fx)
{
    if (! fx.isVST)
        return nullptr;

    auto isVST3 = [] (const juce::PluginDescription& d) { return d.pluginFormatName == "VST3"; };

    // 1. The exact VST3 class (finds a Waves plugin inside its WaveShell, whatever FL called it).
    if (fx.vst3ClassID.getSize() == 16)
    {
        const auto hashes = hashesForClass (fx.vst3ClassID);

        for (auto& d : known)
            if (isVST3 (d) && (hashes.contains (d.uniqueId) || hashes.contains (d.deprecatedUid)))
                return &d;
    }

    // 1b. The exact VST2 (by its ID): FL's saved sound loads into it as it was.
    if (! fx.isVST3 && fx.vst2ID != 0)
        for (auto& d : known)
            if (d.pluginFormatName == "VST" && d.uniqueId == fx.vst2ID)
                return &d;

    // 2. By name: FL's name for it, the plugin's own, the file's, the one inside its state.
    juce::StringArray names;
    const bool shell = fx.pluginName.containsIgnoreCase ("WaveShell") || fx.file.containsIgnoreCase ("WaveShell");

    names.add (normalise (fx.displayName));
    names.add (normalise (nameInsideState (fx.componentState)));

    if (! shell)
    {
        names.add (normalise (fx.pluginName));
        names.add (normalise (juce::File::createFileWithoutCheckingPath (fx.file.replaceCharacter ('\\', '/')).getFileNameWithoutExtension()));
    }

    names.removeEmptyStrings();
    names.removeDuplicates (false);

    const juce::PluginDescription* best = nullptr;
    int bestScore = 0;
    const auto vendor = normalise (fx.vendor);

    for (auto& d : known)
    {
        const auto n = normalise (d.name);

        for (auto& wanted : names)
        {
            int score = 0;

            if (n == wanted)
                score = 100;
            else if (wanted.length() >= 3 && n.startsWith (wanted))
                score = 60 - (n.length() - wanted.length());     // "oneknobbrighter" -> "oneknobbrighterstereo", "j37" -> "j37stereo"
            else if (n.length() >= 5 && wanted.startsWith (n))
                score = 40;

            if (score <= 0)
                continue;

            if (isVST3 (d) == fx.isVST3)
                score += 10;                                     // the version FL used (its sound loads as it was), if there is a choice

            if (vendor.isNotEmpty() && normalise (d.manufacturerName).contains (vendor.substring (0, 5)))
                score += 5;

            if (n.endsWith ("stereo"))
                score += 1;                                      // Waves: stereo over mono

            if (score > bestScore)
            {
                bestScore = score;
                best = &d;
            }
        }
    }

    return best;
}

//==============================================================================
namespace
{
    /** FL's numbers for one of its own effects (32-bit each). */
    std::vector<int> flValues (const juce::MemoryBlock& data)
    {
        std::vector<int> v;
        auto* d = (const juce::uint8*) data.getData();

        for (size_t i = 0; i + 4 <= data.getSize(); i += 4)
            v.push_back ((int) juce::ByteOrder::littleEndianInt (d + i));

        return v;
    }

    /** The tempo division nearest to a length in steps (1/16 notes). */
    int nearestDivision (double steps)
    {
        int best = 0;

        for (int i = 1; i < dly::numDivisions; ++i)
            if (std::abs (std::log (dly::divisions[i].steps / steps)) < std::abs (std::log (dly::divisions[best].steps / steps)))
                best = i;

        return best;
    }

    void setValue (te::Plugin& p, const char* id, double value)
    {
        if (auto param = p.getAutomatableParameterByID (id))
            param->setParameter ((float) juce::jlimit ((double) param->valueRange.start, (double) param->valueRange.end, value),
                                 juce::sendNotification);
    }

    /** For knobs whose units FL doesn't show: the same place on our knob (0..1). */
    void setKnob (te::Plugin& p, const char* id, double proportion)
    {
        if (auto param = p.getAutomatableParameterByID (id))
            param->setNormalisedParameter ((float) juce::jlimit (0.0, 1.0, proportion), juce::sendNotification);
    }
}

bool applyFlSettings (te::Plugin& p, const Effect& fx, double bpm)
{
    const auto v = flValues (fx.flData);
    const double secondsPerStep = 15.0 / juce::jmax (10.0, bpm > 0.0 ? bpm : 120.0);

    if (fx.kind == "Fruity Delay 3" && dynamic_cast<DelayPlugin*> (&p) != nullptr)
    {
        if (v.size() < 27 || v[0] != 1)
            return false;

        auto knob = [&v] (int i) { return v[(size_t) i] / 6000.0; };
        const double steps = juce::jmax (1, v[5]) / 48.0;

        setValue (p, "input", knob (1));
        setValue (p, "model", juce::jlimit (0, 3, v[2]));          // mono, stereo, ping pong, off: the same order here
        setValue (p, "sync", v[3] != 0 ? 1.0 : 0.0);
        setValue (p, "keepPitch", v[4] != 0 ? 1.0 : 0.0);
        setValue (p, "division", nearestDivision (steps));
        setValue (p, "time", steps * secondsPerStep * 1000.0);     // (without tempo sync: the same length at the song's tempo)
        setValue (p, "smoothing", knob (6));
        setValue (p, "offset", (v[7] - 500) / 500.0);
        setValue (p, "spread", knob (8));
        setKnob  (p, "modRate", knob (9));
        setValue (p, "modTime", knob (10));
        setValue (p, "modCutoff", knob (11));
        setValue (p, "diffusion", knob (12));
        setValue (p, "diffusionSpread", knob (13));
        setValue (p, "distType", v[14] != 0 ? dly::saturate : dly::limit);
        setKnob  (p, "feedback", knob (15));

        static constexpr int filters[] = { dly::lowPass, dly::highPass, dly::bandPass, dly::filterOff };   // FL: LP, HP, BP, off
        setValue (p, "filterType", filters[juce::jlimit (0, 3, v[16])]);
        setKnob  (p, "cutoff", knob (17));
        setValue (p, "resonance", knob (18));
        setValue (p, "distLevel", -30.0 * knob (19));               // knob turned up = distorts sooner
        setValue (p, "distKnee", knob (20));
        setValue (p, "distSymmetry", knob (21));
        setKnob  (p, "sampleRate", v[22] / 60000.0);
        setKnob  (p, "bits", knob (23));
        setValue (p, "wet", knob (24));
        setValue (p, "tone", v[25] / 3000.0);
        setValue (p, "dry", knob (26));
        return true;
    }

    if (fx.kind == "Fruity Reeverb 2" && dynamic_cast<ReverbPlugin*> (&p) != nullptr)
    {
        if (v.size() < 16 || v[0] != 10001)
            return false;

        auto* bytes = (const juce::uint8*) fx.flData.getData();
        const bool tempo = fx.flData.getSize() >= 66 && bytes[64] != 0;
        const bool side = fx.flData.getSize() >= 66 && bytes[65] != 0;
        const double steps = v[3] / 48.0;

        setValue (p, "lowCut", v[1]);
        setValue (p, "highCut", v[2] * 100.0);
        setValue (p, "tempo", tempo ? 1.0 : 0.0);
        setValue (p, "predelay", steps * secondsPerStep * 1000.0);
        setValue (p, "predelayDivision", nearestDivision (juce::jmax (0.25, steps)));
        setValue (p, "size", v[4] / 100.0);
        setValue (p, "diffusion", v[5] / 100.0);
        setValue (p, "decay", v[6] / 10.0);
        setValue (p, "damp", v[7] * 100.0);                          // FL's maximum: no damping, ours too
        setValue (p, "bass", v[8] / 100.0);
        setValue (p, "crossover", v[9]);
        setValue (p, "mod", v[10] / 64.0);
        setValue (p, "dry", v[11] / 128.0);
        setValue (p, "early", v[12] / 128.0);
        setValue (p, "wet", v[13] / 128.0);
        setKnob  (p, "modSpeed", v[14] / 2048.0);
        setValue (p, "side", side ? 1.0 : 0.0);
        return true;
    }

    if (fx.kind == "Fruity Parametric EQ 2" && dynamic_cast<ParametricEQPlugin*> (&p) != nullptr)
    {
        if (v.size() < 37 || v[0] != 8)
            return false;

        // FL's band types: off, low pass, band pass, high pass, notch, low shelf, peaking, high shelf.
        static constexpr int types[] = { peq::off, peq::lowPass, peq::bandPass, peq::highPass, peq::notch,
                                         peq::lowShelf, peq::peak, peq::highShelf };

        for (int b = 0; b < 7 && b < peq::numBands; ++b)
        {
            const juce::String n (b + 1);
            const int type = types[juce::jlimit (0, 7, v[(size_t) (22 + b)])];
            const double width = juce::jlimit (0.02, 1.0, v[(size_t) (15 + b)] / 65536.0);
            const double octaves = width * 4.0;                       // FL's default peak: about 1.5 octaves
            const double q = std::sqrt (std::pow (2.0, octaves)) / (std::pow (2.0, octaves) - 1.0);

            setValue (p, ("type" + n).toRawUTF8(), type);
            setValue (p, ("gain" + n).toRawUTF8(), v[(size_t) (1 + b)] / 100.0);
            setValue (p, ("freq" + n).toRawUTF8(), 20.0 * std::pow (1000.0, juce::jlimit (0, 65536, v[(size_t) (8 + b)]) / 65536.0));
            setValue (p, ("q" + n).toRawUTF8(), (type == peq::lowShelf || type == peq::highShelf) && v[(size_t) (15 + b)] == 40124
                                                    ? 0.707 : q);   // FL's default shelf: our default shelf
            setValue (p, ("order" + n).toRawUTF8(), juce::jlimit (0, 3, v[(size_t) (29 + b)]));
        }

        setValue (p, "main", v[36] / 100.0);
        return true;
    }

    if (fx.kind == "Fruity Limiter" && dynamic_cast<LimiterPlugin*> (&p) != nullptr)
    {
        // Version 7: limiter gain, saturation, ceiling, attack, (curve), release, (curve), sustain,
        // then the compressor's threshold, knee, ratio, attack, release, (curve), sustain, then the
        // gate's gain, threshold, release. Levels: 1000 = 0 dB (x1); times 0..10000 (knob position).
        if (v.size() < 19 || v[0] != 7)
            return false;

        auto db = [] (int x) { return x <= 0 ? -100.0 : 20.0 * std::log10 (x / 1000.0); };
        auto knob = [] (int x) { return juce::jlimit (0.0, 1.0, x / 10000.0); };

        setValue (p, "limGain", db (v[1]));
        setKnob  (p, "saturation", 1.0 - v[2] / 1000.0);        // FL: 1000 = none
        setValue (p, "ceiling", db (v[3]));
        setKnob  (p, "limAttack", knob (v[4]));
        setKnob  (p, "limRelease", knob (v[6]));
        setKnob  (p, "limSustain", knob (v[8]));
        setValue (p, "compThresh", db (v[9]));
        setValue (p, "compKnee", 24.0 * v[10] / 1000.0);
        setValue (p, "compRatio", 1.0 + 29.0 * std::pow (juce::jlimit (0.0, 1.0, v[11] / 1000.0), 2.0));
        setKnob  (p, "compAttack", knob (v[12]));
        setKnob  (p, "compRelease", knob (v[13]));
        setKnob  (p, "compSustain", knob (v[15]));
        setValue (p, "gateGain", juce::jlimit (0.0, 1.0, v[16] / 1000.0));
        setValue (p, "gateThresh", juce::jmax (-80.0, db (v[17])));
        setKnob  (p, "gateRelease", knob (v[18]));
        return true;
    }

    if (fx.kind == "Fruity Compressor" && dynamic_cast<CompressorFxPlugin*> (&p) != nullptr)
    {
        // Version 3: threshold (0.1 dB), ratio (x10), gain (0.1 dB), attack (0.1 ms), release (ms), type.
        if (v.size() < 7 || v[0] != 3)
            return false;

        setValue (p, "threshold", v[1] / 10.0);
        setValue (p, "ratio", v[2] / 10.0);
        setValue (p, "gain", v[3] / 10.0);
        setValue (p, "attack", v[4] / 10.0);
        setValue (p, "release", v[5]);
        setValue (p, "compType", juce::jlimit (0, 7, v[6]));
        return true;
    }

    if (fx.kind == "Soundgoodizer" && dynamic_cast<EnhancerPlugin*> (&p) != nullptr)
    {
        // Version 3: mode (0 = A ... 3 = D), amount (0..1000).
        if (v.size() < 3 || v[0] != 3)
            return false;

        setValue (p, "mode", juce::jlimit (0, 3, v[1]));
        setValue (p, "amount", v[2] / 1000.0);
        return true;
    }

    if (fx.kind == "Fruity Flangus" && dynamic_cast<FlangerPlugin*> (&p) != nullptr)
    {
        // Order, depth (0..1024), speed (0..1024), delay (0..128), spread (0..128), cross, dry, wet (-128..128).
        if (v.size() < 8)
            return false;

        setValue (p, "order", juce::jlimit (1, FlangerPlugin::maxOrder, v[0]));
        setValue (p, "depth", v[1] / 1024.0);
        setValue (p, "speed", v[2] / 1024.0);
        setValue (p, "delay", v[3] / 128.0);
        setValue (p, "spread", v[4] / 128.0);
        setValue (p, "cross", v[5] / 128.0);
        setValue (p, "dry", v[6] / 128.0);
        setValue (p, "wet", v[7] / 128.0);
        return true;
    }

    if (fx.kind == "Fruity Love Philter" && dynamic_cast<FilterFxPlugin*> (&p) != nullptr)
    {
        // u32 version, then zlib: ..., byte 62 = length of a UTF-16 text (preset name), the
        // text, then the first unit: knobs 0..256 (128 = 50%, volumes 128 = 100%), the low /
        // band / high sliders (0..128), ..., the filter type (0 off, 1 single, 2 dual,
        // 3 triple, 4 alt dual, 5 alt triple). Only the first unit comes over.
        if (fx.flData.getSize() < 8)
            return false;

        juce::MemoryOutputStream unpacked;

        {
            juce::MemoryInputStream packed (juce::addBytesToPointer (fx.flData.getData(), 4), fx.flData.getSize() - 4, false);
            juce::GZIPDecompressorInputStream zlib (&packed, false, juce::GZIPDecompressorInputStream::zlibFormat);
            unpacked.writeFromInputStream (zlib, 1 << 20);
        }

        auto* d = (const juce::uint8*) unpacked.getData();
        const auto size = unpacked.getDataSize();

        if (size < 64)
            return false;

        const size_t start = 63 + 2 * (size_t) d[62];

        if (start + 14 * 4 > size)
            return false;

        auto at = [d, start] (int i) { return (int) juce::ByteOrder::littleEndianInt (d + start + 4 * (size_t) i); };
        auto gainDb = [] (int x) { return x <= 0 ? -24.0 : 20.0 * std::log10 (x / 128.0); };
        static constexpr int stageCounts[] = { 0, 1, 2, 3, 2, 3 };

        setValue (p, "inVol", gainDb (at (0)));
        setKnob  (p, "cutoff", at (2) / 256.0);
        setKnob  (p, "resonance", juce::jlimit (0.0, 1.0, (at (6) - 64) / 128.0));
        setValue (p, "low", at (3) / 128.0);
        setValue (p, "band", at (4) / 128.0);
        setValue (p, "high", at (5) / 128.0);
        setValue (p, "outVol", gainDb (at (9)));
        setValue (p, "stages", stageCounts[juce::jlimit (0, 5, at (12))]);
        return true;
    }

    return false;
}

juce::MemoryBlock makeVST2State (const juce::MemoryBlock& chunk, int vst2ID)
{
    // An .fxb "bank chunk" (what JUCE's VST2 host reads back): big-endian header, then the chunk.
    juce::MemoryOutputStream out;
    auto be = [&out] (juce::uint32 x) { out.writeIntBigEndian ((int) x); };

    out.write ("CcnK", 4);
    be ((juce::uint32) (chunk.getSize() + 152));   // the size after these 8 bytes
    out.write ("FBCh", 4);
    be (1);                                         // version (JUCE reads up to 1)
    be ((juce::uint32) vst2ID);
    be (1);                                         // the plugin's version (not checked)
    be (1);                                         // programs
    out.writeRepeatedByte (0, 128);                 // (future)
    be ((juce::uint32) chunk.getSize());
    out.write (chunk.getData(), chunk.getSize());
    return out.getMemoryBlock();
}

juce::MemoryBlock makeVST3State (const juce::MemoryBlock& component, const juce::MemoryBlock& controller)
{
    juce::XmlElement state ("VST3PluginState");

    if (component.getSize() > 0)
        state.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());

    if (controller.getSize() > 0)
        state.createNewChildElement ("IEditController")->addTextElement (controller.toBase64Encoding());

    juce::MemoryBlock result;
    juce::AudioProcessor::copyXmlToBinary (state, result);
    return result;
}

namespace
{
    /** Puts FL's saved settings of a VST into the plugin about to be made. Returns true if
        it could (the VST3 with its VST3 state; the VST2 with its chunk; or the VST3 given
        the VST2's chunk, which many plugins read too). */
    bool putFlState (juce::ValueTree& tree, const juce::PluginDescription& desc, const Effect& fx)
    {
        if (fx.componentState.getSize() == 0)
            return false;

        if (desc.pluginFormatName == "VST3")
            tree.setProperty (te::IDs::state, makeVST3State (fx.componentState, fx.controllerState).toBase64Encoding(), nullptr);
        else if (desc.pluginFormatName == "VST" && fx.stateFromVST2)
            tree.setProperty (te::IDs::state, makeVST2State (fx.componentState, fx.vst2ID != 0 ? fx.vst2ID : desc.uniqueId).toBase64Encoding(), nullptr);
        else
            return false;

        return true;
    }

    /** FL used the VST2 and its settings went to the VST3 (they may not sound the same). */
    bool vst2SettingsInVST3 (const juce::PluginDescription& desc, const Effect& fx)
    {
        return fx.stateFromVST2 && desc.pluginFormatName == "VST3";
    }
}

juce::String describeFile (const juce::File& file)
{
    Project p;
    const auto error = parse (file, p);

    if (error.isNotEmpty())
        return "error: " + error;

    juce::StringArray lines;
    lines.add ("FL Studio " + p.flVersion + ", " + juce::String (p.bpm, 2) + " BPM, "
               + juce::String (p.numerator) + "/" + juce::String (p.denominator) + ", " + juce::String (p.inserts.size()) + " mixer tracks");

    juce::StringArray tracks;
    for (int i = 0; i < p.trackNames.size(); ++i)
        if (p.trackNames[i].isNotEmpty())
            tracks.add (juce::String (i + 1) + " " + p.trackNames[i]);
    lines.add ("playlist: " + tracks.joinIntoString (", "));

    if (p.hasBeat())
    {
        p.findSamples (file);
        size_t notes = 0;
        for (auto& pat : p.patterns) notes += pat.notes.size();

        lines.add ("beat: " + juce::String ((int) p.channels.size()) + " channels, " + juce::String ((int) p.patterns.size()) + " patterns ("
                   + juce::String ((int) notes) + " notes), " + juce::String ((int) p.items.size()) + " playlist items, ppq " + juce::String (p.ppq)
                   + ", sounds found " + juce::String (p.countSamples (true)) + "/" + juce::String (p.countSamples (false)));

        for (auto& c : p.channels)
            lines.add ("  ch " + juce::String (c.index) + " [" + juce::String (c.type) + "] " + c.name
                       + (c.kind.isNotEmpty() ? " (" + c.kind + (c.plugin.pluginName.isNotEmpty() ? ": " + c.plugin.pluginName : juce::String()) + ")" : juce::String())
                       + " -> insert " + juce::String (c.insert) + ", vol " + juce::String (c.volume, 2) + ", pan " + juce::String (c.pan, 2)
                       + (c.volumeEnvelope ? ", note length" : "")
                       + (c.samplePath.isNotEmpty() ? ", sound " + (c.sampleFile.existsAsFile() ? c.sampleFile.getFullPathName() : "NOT FOUND: " + c.samplePath) : juce::String()));
    }

    auto used = p.getUsedInserts();
    used.insert (0, 0);

    for (auto n : used)
    {
        auto& ins = p.inserts.getReference (n);
        juce::StringArray routes;

        for (auto& r : ins.routes)
            routes.add (p.inserts[r.to].displayName() + (r.closed ? " (closed)" : " (" + juce::String (r.db, 1) + " dB)"));

        lines.add ("[" + juce::String (n) + "] " + ins.displayName() + "  fader " + juce::String (ins.volumeDb, 1) + " dB, pan "
                   + juce::String (ins.pan, 2) + "  -> " + (routes.isEmpty() ? juce::String ("-") : routes.joinIntoString (", ")));

        for (auto& fx : ins.effects)
            lines.add ("     " + juce::String (fx.slot) + ": " + fx.describe() + (fx.isVST ? (fx.isVST3 ? " [VST3" : " [VST2") + juce::String (fx.componentState.getSize() > 0 ? ", state " + juce::String ((int) fx.componentState.getSize()) + "+" + juce::String ((int) fx.controllerState.getSize()) + " bytes" : juce::String()) + "]" : juce::String())
                       + (fx.enabled ? "" : " OFF") + (fx.mix < 0.995 ? " mix " + juce::String (juce::roundToInt (fx.mix * 100)) + "%" : juce::String()));
    }

    return lines.joinIntoString ("\n");
}

juce::String dryRun (te::Engine& engine, const juce::File& file, const juce::StringArray& args)
{
    Project p;
    const auto error = parse (file, p);

    if (error.isNotEmpty())
        return "error: " + error;

    juce::StringArray skip;

    for (auto& a : args)
        if (a.startsWith ("--flp-skip="))
            skip.addTokens (a.fromFirstOccurrenceOf ("=", false, false).unquoted(), "|", {});

    Options options;
    options.skipOffEffects = args.contains ("--flp-skip-off");

    for (auto n : p.getUsedInserts())
        if (! skip.contains (p.inserts[n].displayName(), true))
            options.inserts.add (n);

    auto edit = te::createEmptyEdit (engine, juce::File());

    for (auto* t : te::getAudioTracks (*edit))
        edit->deleteTrack (t);

    // (No sounds are copied in a dry run: no song audio store here.)
    const auto report = apply (*edit, p, options);
    juce::StringArray lines;
    lines.add (report.toText());

    if (p.hasBeat())
    {
        lines.add ({});

        for (auto& pattern : song::getPatterns (*edit))
        {
            juce::StringArray perChannel;

            for (auto* t : song::getChannels (*edit))
                if (auto n = song::getNotes (pattern, song::getChannelID (*t)).size(); n > 0)
                    perChannel.add (t->getName() + " " + juce::String (n));

            lines.add ("pattern \"" + pattern[song::ids::name].toString() + "\" " + juce::String (song::getBars (pattern)) + " bars: " + perChannel.joinIntoString (", "));
        }

        juce::StringArray clips;
        for (auto& c : song::getClips (*edit))
            if (clips.size() < 12)
                clips.add ("track " + juce::String ((int) c[song::ids::lane] + 1) + " @" + juce::String ((double) c[song::ids::start], 2)
                           + " len " + juce::String (song::getClipLength (*edit, c), 2) + (c.hasProperty (song::ids::poff) ? " from " + c[song::ids::poff].toString() : juce::String()));
        lines.add ("clips: " + juce::String (song::getClips (*edit).size()) + " (first: " + clips.joinIntoString ("; ") + ")");
    }
    lines.add ({});
    lines.add (juce::String (edit->tempoSequence.getTempo (0)->getBpm(), 2) + " BPM");

    auto effectList = [] (const juce::Array<te::Plugin*>& effects)
    {
        juce::StringArray names;
        for (auto* fx : effects)
            names.add (fx->getName() + (fx->isEnabled() ? "" : " (off)"));
        return names.joinIntoString (", ");
    };

    lines.add ("Master: " + effectList (song::getMasterEffects (*edit)));

    for (auto* t : song::getInserts (*edit))
    {
        auto* to = song::resolveRoute (*edit, *t);
        juce::StringArray sends;

        for (auto& s : song::getSendsFrom (*edit, t->itemID.toString()))
            if (auto* target = song::findInsert (*edit, s[song::ids::to].toString()))
                sends.add (target->getName() + " " + (song::getSendLevel (s) <= song::minSendDb ? juce::String ("off") : juce::String (song::getSendLevel (s), 1) + " dB"));

        lines.add (t->getName() + "  fader " + juce::String (song::getVolumePlugin (*t)->getVolumeDb(), 1) + " dB -> "
                   + (to != nullptr ? to->getName() : juce::String ("Master"))
                   + (sends.isEmpty() ? juce::String() : "  sends: " + sends.joinIntoString (", ")));

        const auto fx = effectList (song::getInsertEffects (*t));
        if (fx.isNotEmpty())
            lines.add ("     " + fx);

        // Our own effects: every knob (to check what FL's settings became).
        for (auto* e : song::getInsertEffects (*t))
            if (auto* coto = dynamic_cast<CotoFxPlugin*> (e))
            {
                juce::StringArray knobs;

                for (auto* param : coto->params())
                    knobs.add (param->getParameterName() + " " + param->getCurrentValueAsString());

                lines.add ("       " + e->getName() + ": " + knobs.joinIntoString (", "));
            }
            else if (auto* eq = dynamic_cast<ParametricEQPlugin*> (e))
            {
                juce::StringArray knobs;

                for (auto* param : eq->getAutomatableParameters())
                    if (! param->paramID.startsWith ("order"))
                        knobs.add (param->getParameterName() + " " + param->getCurrentValueAsString());

                lines.add ("       " + e->getName() + ": " + knobs.joinIntoString (", "));
            }
    }

    juce::StringArray names;
    for (int i = 0; i < song::numLanes; ++i)
        if (song::getLaneName (*edit, i).isNotEmpty())
            names.add (juce::String (i + 1) + " " + song::getLaneName (*edit, i));

    lines.add ("Playlist: " + names.joinIntoString (", "));
    return lines.joinIntoString ("\n");
}

//==============================================================================
juce::String Report::toText() const
{
    juce::StringArray lines;
    lines.add (TRANS ("{N} insert(s), {E} effect(s) ({S} with their FL settings), {X} send(s).")
                 .replace ("{N}", juce::String (insertsMade)).replace ("{E}", juce::String (effectsLoaded))
                 .replace ("{S}", juce::String (effectsWithSettings)).replace ("{X}", juce::String (sendsMade)));

    if (channelsMade + patternsMade + clipsMade > 0)
        lines.add (TRANS ("The beat: {C} channel(s), {P} pattern(s) with {N} note(s), {L} clip(s) on the playlist.")
                     .replace ("{C}", juce::String (channelsMade)).replace ("{P}", juce::String (patternsMade))
                     .replace ("{N}", juce::String (notesMade)).replace ("{L}", juce::String (clipsMade)));

    auto section = [&lines] (const juce::String& title, const juce::StringArray& items)
    {
        if (items.isEmpty())
            return;

        lines.add ({});
        lines.add (title);

        for (auto& i : items)
            lines.add ("  - " + i);
    };

    section (TRANS ("Sounds not found (their channels are there, silent: drop the sound on the channel, or import again and show where they are):"), missingSounds);
    section (TRANS ("Instruments not here (their channels keep the notes: put an instrument on them):"), instrumentsMissing);
    section (TRANS ("Not installed on this PC (left out; scan your plugins, then import again):"), notInstalled);
    section (TRANS ("FL Studio effects COTOSTUDIO doesn't have yet (left out):"), notAvailable);
    section (TRANS ("Loaded with their default settings:"), defaultSettings);
    section (TRANS ("Left out because they were off in FL:"), leftOutOff);
    section (TRANS ("Good to know:"), notes);
    return lines.joinIntoString ("\n");
}

void applyBeat (te::Edit&, const Project&, const Options&, const std::map<int, te::AudioTrack*>& inserts, Report&);

/** VST2s FL used that haven't been scanned here: if their file is where FL had it, they
    are added to the plugin list (so a beat with Omnisphere loads without a full scan). */
static void scanFlVST2s (te::Engine& engine, const Project& project)
{
    auto& pm = engine.getPluginManager();
    juce::AudioPluginFormat* vst2 = nullptr;

    for (auto* f : pm.pluginFormatManager.getFormats())
        if (f->getName() == "VST")
            vst2 = f;

    if (vst2 == nullptr)
        return;

    auto check = [&] (const Effect& fx)
    {
        if (! fx.isVST || fx.isVST3 || fx.file.isEmpty() || ! juce::File::isAbsolutePath (fx.file))
            return;

        for (auto& d : pm.knownPluginList.getTypes())
            if (d.pluginFormatName == "VST" && (d.uniqueId == fx.vst2ID || d.fileOrIdentifier == fx.file))
                return;

        if (juce::File (fx.file).existsAsFile())
        {
            juce::OwnedArray<juce::PluginDescription> found;
            pm.knownPluginList.scanAndAddFile (fx.file, true, found, *vst2);
        }
    };

    for (auto& ins : project.inserts)
        for (auto& fx : ins.effects)
            check (fx);

    for (auto& c : project.channels)
        check (c.plugin);
}

/** COTOSTUDIO's own effect standing in for one of FL's: { type, name }, or empty. */
static std::pair<juce::String, juce::String> ourVersionOf (const juce::String& flKind)
{
    if (flKind == "Fruity Parametric EQ 2")  return { ParametricEQPlugin::xmlTypeName, TRANS ("Parametric EQ") };
    if (flKind == "Fruity Limiter")          return { LimiterPlugin::xmlTypeName, TRANS ("Limiter") };
    if (flKind == "Fruity Delay 3")          return { DelayPlugin::xmlTypeName, TRANS ("Delay") };
    if (flKind == "Fruity Reeverb 2")        return { ReverbPlugin::xmlTypeName, TRANS ("Reverb") };
    if (flKind == "Fruity Compressor")       return { CompressorFxPlugin::xmlTypeName, TRANS ("Compressor") };
    if (flKind == "Soundgoodizer")           return { EnhancerPlugin::xmlTypeName, TRANS ("Enhancer") };
    if (flKind == "Fruity Flangus")          return { FlangerPlugin::xmlTypeName, TRANS ("Flanger") };
    if (flKind == "Fruity Love Philter")     return { FilterFxPlugin::xmlTypeName, TRANS ("Filter") };
    return {};
}

Report apply (te::Edit& edit, const Project& project, const Options& options)
{
    Report report;
    scanFlVST2s (edit.engine, project);
    const auto known = edit.engine.getPluginManager().knownPluginList.getTypes();
    std::map<int, te::AudioTrack*> made;   // FL insert number -> our insert

    auto chosen = options.inserts;
    chosen.removeAllInstancesOf (0);
    chosen.sort();

    // 1. The inserts, in FL's order, with their faders.
    for (auto n : chosen)
    {
        if (! juce::isPositiveAndBelow (n, project.inserts.size()))
            continue;

        if (auto* t = song::addInsert (edit, project.inserts[n].displayName()))
        {
            made[n] = t;
            ++report.insertsMade;
        }
    }

    auto setFader = [&] (te::VolumeAndPanPlugin* vol, double db, double pan, const juce::String& who)
    {
        if (vol == nullptr)
            return;

        if (db > 6.0)
            report.notes.add (TRANS ("{NAME}: fader at +6 dB (the most here; FL had {DB} dB with its route level)")
                                .replace ("{NAME}", who).replace ("{DB}", "+" + juce::String (db, 1)));

        vol->setVolumeDb ((float) juce::jlimit (-100.0, 6.0, db));
        vol->setPan ((float) pan);
    };

    // 2. Routing: one route out (the master if FL sends there, otherwise the loudest
    //    destination, its level added to the fader); every other destination is a send.
    struct PendingSend { juce::String from, to; double db; juce::String what; };
    juce::Array<PendingSend> sends;

    for (auto& [n, track] : made)
    {
        auto& ins = project.inserts.getReference (n);
        juce::Array<Route> routes;

        for (auto& r : ins.routes)
            if (r.to == 0 || made.count (r.to) > 0)
                routes.add (r);

        double faderDb = ins.volumeDb;
        int mainRoute = 0;

        if (! routes.isEmpty() && ! ins.routesTo (0))
        {
            int best = 0;

            for (int i = 1; i < routes.size(); ++i)
                if (routes[i].db > routes[best].db)
                    best = i;

            mainRoute = routes[best].to;
            song::setRoute (edit, *track, made[mainRoute]->itemID.toString());

            if (! routes[best].closed)
                faderDb += routes[best].db;
        }
        else if (routes.isEmpty() && ! ins.routes.isEmpty())
        {
            report.notes.add (TRANS ("{NAME}: everything it fed was left out, so it goes to the master")
                                .replace ("{NAME}", ins.displayName()));
        }

        setFader (song::getVolumePlugin (*track), faderDb, ins.pan, ins.displayName());

        for (auto& r : routes)
            if (r.to != 0 && r.to != mainRoute)
                sends.add ({ track->itemID.toString(), made[r.to]->itemID.toString(),
                             r.closed ? song::minSendDb : r.db,
                             ins.displayName() + " -> " + project.inserts[r.to].displayName() });
    }

    for (auto& s : sends)
    {
        if (song::addSend (edit, s.from, s.to, s.db).isValid())
            ++report.sendsMade;
        else
            report.notes.add (TRANS ("Send left out (it would make a loop): {WHAT}").replace ("{WHAT}", s.what));
    }

    // 3. Effects, slot by slot (VST3 with FL's saved settings).
    auto addEffects = [&] (const Insert& ins, te::AudioTrack* track)
    {
        for (auto& fx : ins.effects)
        {
            const auto where = fx.describe() + " (" + ins.displayName() + ")";
            te::Plugin::Ptr plugin;

            if (! fx.enabled && options.skipOffEffects)
            {
                report.leftOutOff.add (where);
                continue;
            }

            if (fx.isVST)
            {
                auto* desc = findPlugin (known, fx);

                if (desc == nullptr)
                {
                    report.notInstalled.add (where + (fx.missingInFL ? " - " + TRANS ("FL couldn't load it either") : juce::String()));
                    continue;
                }

                auto tree = te::ExternalPlugin::create (edit.engine, *desc);
                const bool canUseState = putFlState (tree, *desc, fx);

                plugin = edit.getPluginCache().createNewPlugin (tree);

                if (plugin != nullptr)
                {
                    if (canUseState)
                    {
                        ++report.effectsWithSettings;

                        if (vst2SettingsInVST3 (*desc, fx))
                            report.notes.add (TRANS ("{WHAT}: FL used the VST2; its settings were given to the VST3 (check it sounds right)").replace ("{WHAT}", where));
                    }
                    else if (fx.missingInFL)
                        report.defaultSettings.add (where + " - " + TRANS ("FL couldn't load it either, so no settings were saved"));
                    else
                        report.defaultSettings.add (where);

                    if (fx.mix < 0.995)
                        report.notes.add (TRANS ("{WHAT}: FL mixed it in at {P}% - here it is at 100%")
                                            .replace ("{WHAT}", where).replace ("{P}", juce::String (juce::roundToInt (fx.mix * 100.0))));
                }
            }
            else if (auto ours = ourVersionOf (fx.kind); ours.first.isNotEmpty())
            {
                // Our own versions, with FL's settings where they can be read.
                plugin = song::createEffect (edit, ours.first, nullptr);

                if (plugin != nullptr)
                {
                    if (applyFlSettings (*plugin, fx, project.bpm))
                    {
                        ++report.effectsWithSettings;
                        report.notes.add (TRANS ("{OURS} (ours) in place of {FL}, with your settings from FL")
                                            .replace ("{OURS}", ours.second).replace ("{FL}", fx.kind) + " (" + ins.displayName() + ")");
                    }
                    else
                    {
                        report.defaultSettings.add (TRANS ("{OURS} (ours) in place of {FL} - set it again")
                                                      .replace ("{OURS}", ours.second).replace ("{FL}", fx.kind) + " (" + ins.displayName() + ")");
                    }

                    // FL's slot mix (dry sound + this much of the effect's output) goes into our
                    // dry / wet knobs where the effect has them.
                    auto dryParam = plugin->getAutomatableParameterByID ("dry"), wetParam = plugin->getAutomatableParameterByID ("wet");

                    if (fx.mix < 0.995 && dryParam != nullptr && wetParam != nullptr)
                    {
                        const auto m = (float) juce::jlimit (0.0, 1.0, fx.mix);
                        dryParam->setParameter ((1.0f - m) + m * dryParam->getCurrentValue(), juce::sendNotification);
                        wetParam->setParameter (m * wetParam->getCurrentValue(), juce::sendNotification);
                    }
                    else if (fx.mix < 0.995)
                        report.notes.add (TRANS ("{WHAT}: FL mixed it in at {P}% - here it is at 100%")
                                            .replace ("{WHAT}", where).replace ("{P}", juce::String (juce::roundToInt (fx.mix * 100.0))));
                }
            }
            else
            {
                report.notAvailable.add (where + (fx.enabled ? juce::String() : " - " + TRANS ("it was off")));
                continue;
            }

            if (plugin == nullptr)
            {
                report.notInstalled.add (where);
                continue;
            }

            song::putEffect (edit, track, plugin, -1);

            if (! fx.enabled)
            {
                plugin->setEnabled (false);
                report.notes.add (TRANS ("{WHAT}: off (bypassed), as in FL").replace ("{WHAT}", where));
            }

            ++report.effectsLoaded;
        }
    };

    if (! project.inserts.isEmpty())
    {
        auto& master = project.inserts.getReference (0);
        addEffects (master, nullptr);

        if (auto* mv = edit.getMasterVolumePlugin().get())
            setFader (mv, master.volumeDb, master.pan, master.displayName());
    }

    for (auto& [n, track] : made)
        addEffects (project.inserts.getReference (n), track);

    // 4. Tempo and playlist track names.
    if (options.tempo && project.bpm >= 40.0 && project.bpm <= 250.0)
        if (auto* tempo = edit.tempoSequence.getTempo (0))
            tempo->setBpm (project.bpm);

    if (project.numerator != 4 || project.denominator != 4)
    {
        const song::TimeSignature sig { project.numerator, project.denominator };

        if (song::getTimeSignatureChoices().contains (sig))
            song::setTimeSignature (edit, sig);
        else
            report.notes.add (TRANS ("FL's time signature was {SIG}; COTOSTUDIO doesn't have it, the song is in 4/4")
                                .replace ("{SIG}", sig.toString()));
    }

    if (options.namesOnPlaylist)
    {
        // A track named after an insert you left out (e.g. "GRABAR ADLIBS") stays unnamed.
        juce::StringArray leftOut;

        for (int n = 1; n < project.inserts.size(); ++n)
            if (project.inserts[n].name.isNotEmpty() && ! chosen.contains (n))
                leftOut.add (project.inserts[n].name);

        for (int i = 0; i < juce::jmin (project.trackNames.size(), song::numLanes); ++i)
        {
            const auto& name = project.trackNames[i];

            if (name.isEmpty())
                continue;

            if (leftOut.contains (name, true))
                report.notes.add (TRANS ("Playlist track {N} stays unnamed: its insert \"{NAME}\" was left out")
                                    .replace ("{N}", juce::String (i + 1)).replace ("{NAME}", name));
            else
                song::setLaneName (edit, i, name);
        }

        // Track colours (only for tracks that came in): the tracks look like they did in FL.
        for (int i = 0; i < juce::jmin (project.trackColours.size(), song::numLanes); ++i)
        {
            const auto& name = i < project.trackNames.size() ? project.trackNames[i] : juce::String();

            if (! project.trackColours[i].isTransparent() && ! leftOut.contains (name, true))
                song::setLaneColour (edit, i, project.trackColours[i]);
        }
    }

    if (options.beat && project.hasBeat())
        applyBeat (edit, project, options, made, report);

    return report;
}

//==============================================================================
void applyBeat (te::Edit& edit, const Project& project, const Options& options,
                const std::map<int, te::AudioTrack*>& inserts, Report& report)
{
    const auto known = edit.engine.getPluginManager().knownPluginList.getTypes();
    std::map<int, te::AudioTrack*> channels;       // FL channel -> our channel
    std::map<int, juce::String> audioAssets;       // FL audio clip channel -> its file's fingerprint
    std::map<int, AssetStore::ImportResult> imported;

    auto importSound = [&] (const Channel& c) -> AssetStore::ImportResult
    {
        if (options.assets == nullptr || ! c.sampleFile.existsAsFile())
        {
            if (c.samplePath.isNotEmpty())
                report.missingSounds.add (c.samplePath.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false)
                                          + " (" + c.name + ")");
            return {};
        }

        auto r = options.assets->importAudioFile (c.sampleFile);

        if (! r.ok())
            report.missingSounds.add (c.sampleFile.getFileName() + " (" + c.name + "): " + r.error);

        return r;
    };

    auto routeAndLevel = [&] (te::AudioTrack& t, const Channel& c)
    {
        if (c.insert > 0)
        {
            if (auto it = inserts.find (c.insert); it != inserts.end())
                song::setRoute (edit, t, it->second->itemID.toString());
            else
                report.notes.add (TRANS ("{NAME}: its insert {N} was left out, it plays to the master")
                                    .replace ("{NAME}", c.name).replace ("{N}", juce::String (c.insert)));
        }

        if (auto* vol = song::getVolumePlugin (t))
        {
            vol->setVolumeDb ((float) juce::jlimit (-60.0, 6.0, juce::Decibels::gainToDecibels (c.volume, -60.0)));
            vol->setPan ((float) c.pan);
        }
    };

    // 1. Channels, in FL's order.
    for (auto& c : project.channels)
    {
        te::AudioTrack* t = nullptr;

        if (c.type == Channel::sampler)
        {
            const auto r = importSound (c);
            t = r.ok() ? song::addSamplerChannel (edit, r, c.sampleFile.getFileName())
                       : song::addEmptySamplerChannel (edit, c.name);

            if (auto* sampler = dynamic_cast<SamplerPlugin*> (t != nullptr ? song::getInstrument (*t) : nullptr))
                sampler->noteLengthValue = c.volumeEnvelope;   // FL's volume envelope on: the sound ends with its note
        }
        else if (c.type == Channel::generator)
        {
            const juce::PluginDescription* desc = c.kind == "Fruity Wrapper" ? findPlugin (known, c.plugin) : nullptr;

            if (desc != nullptr)
            {
                // With FL's saved settings (the sound you had picked), when they can be read.
                auto tree = te::ExternalPlugin::create (edit.engine, *desc);
                const bool withState = putFlState (tree, *desc, c.plugin);

                t = song::addChannel (edit, edit.getPluginCache().createNewPlugin (tree), c.name);

                if (withState && vst2SettingsInVST3 (*desc, c.plugin))
                    report.notes.add (TRANS ("{NAME}: FL used the VST2; its sound was given to the VST3 (check it sounds right)").replace ("{NAME}", c.name));
                else if (! withState)
                    report.notes.add (TRANS ("{NAME}: loaded with its default sound (FL's settings for it don't carry over)").replace ("{NAME}", c.name));
            }
            else
            {
                // Not here (or FL's own instrument): an empty channel keeps its notes, so you can
                // put another instrument on it.
                t = song::addEmptySamplerChannel (edit, c.name);
                report.instrumentsMissing.add (c.name + " - " + (c.kind == "Fruity Wrapper"
                                                   ? (c.plugin.pluginName.isNotEmpty() ? c.plugin.pluginName + ": " : juce::String()) + TRANS ("not installed on this PC")
                                                   : c.kind + ": " + TRANS ("an FL Studio instrument COTOSTUDIO doesn't have")));
            }
        }
        else if (c.type == Channel::audioClip)
        {
            const auto r = importSound (c);

            if (r.ok())
            {
                song::addAsset (edit, r, c.sampleFile.getFileName());
                t = song::addAudioChannel (edit, c.name);
                audioAssets[c.index] = r.hash;
                imported[c.index] = r;
            }
        }
        else if (c.type == Channel::automation)
        {
            report.notes.add (TRANS ("Automation clip \"{NAME}\" left out (make it again with Automate...)").replace ("{NAME}", c.name));
        }
        else
        {
            report.notes.add (TRANS ("{NAME}: FL's layer channels aren't here; its notes are left out").replace ("{NAME}", c.name));
        }

        if (t == nullptr)
            continue;

        t->setName (c.name.substring (0, 40));
        routeAndLevel (*t, c);
        channels[c.index] = t;
        ++report.channelsMade;
    }

    // 2. Patterns and their notes.
    std::map<int, juce::ValueTree> patterns;
    const double bar = song::getBeatsPerBar (edit);

    for (auto& p : project.patterns)
    {
        // As long as its notes, and as the longest clip of it on the playlist.
        double end = 0.0;

        for (auto& n : p.notes)
            end = juce::jmax (end, n.beat + juce::jmax (n.length, song::stepBeats));

        for (auto& item : project.items)
            if (item.pattern == p.number)
                end = juce::jmax (end, item.patternOffset + item.length);

        const int bars = juce::jlimit (1, song::maxBars, (int) std::ceil (end / bar - 1.0e-9));

        if (end > song::maxBars * bar + 1.0e-6)
            report.notes.add (TRANS ("Pattern \"{NAME}\" is longer than {N} bars: its end is cut off").replace ("{NAME}", p.name).replace ("{N}", juce::String (song::maxBars)));

        auto pattern = song::addPattern (edit, p.name.isNotEmpty() ? p.name : "Pattern " + juce::String (p.number), bars);
        patterns[p.number] = pattern;
        ++report.patternsMade;

        for (auto& n : p.notes)
        {
            auto it = channels.find (n.channel);

            if (it == channels.end() || song::isAudioChannel (*it->second) || n.beat >= bars * bar)
                continue;

            auto note = song::addNote (edit, pattern, song::getChannelID (*it->second), n.key, n.beat,
                                       juce::jlimit (song::stepBeats / 4.0, bars * bar - n.beat, n.length), n.velocity);

            if (! note.isValid())
                continue;

            if (n.pan != 0)       song::setNoteProperty (edit, note, song::NoteProperty::pan, n.pan);
            if (n.fine != 0)      song::setNoteProperty (edit, note, song::NoteProperty::fine, n.fine);
            if (n.release != 64)  song::setNoteProperty (edit, note, song::NoteProperty::release, n.release);
            ++report.notesMade;
        }
    }

    // 3. The playlist: pattern clips and audio clips where FL had them.
    int automationItems = 0, beyond = 0;

    for (auto& item : project.items)
    {
        if (item.track >= song::numLanes)
        {
            ++beyond;
            continue;
        }

        if (item.pattern > 0)
        {
            auto it = patterns.find (item.pattern);

            if (it == patterns.end())
                continue;

            auto clip = song::addClip (edit, it->second[song::ids::sid].toString(), item.track, item.start);

            if (clip.isValid())
            {
                clip.setProperty (song::ids::length, item.length, nullptr);

                if (item.patternOffset > 0.0)
                    clip.setProperty (song::ids::poff, item.patternOffset, nullptr);

                ++report.clipsMade;
            }

            continue;
        }

        auto asset = audioAssets.find (item.channel);

        if (asset == audioAssets.end())
        {
            if (auto* c = project.findChannel (item.channel); c != nullptr && c->type == Channel::automation)
                ++automationItems;

            continue;
        }

        auto* ch = channels[item.channel];
        const double seconds = song::beatsToSeconds (edit, item.start + item.length) - song::beatsToSeconds (edit, item.start);
        const double offset = juce::jlimit (0.0, juce::jmax (0.0, imported[item.channel].lengthSeconds - 0.01), item.audioOffset);
        const double duration = juce::jmin (seconds, imported[item.channel].lengthSeconds - offset);

        if (ch != nullptr && duration > 0.0
             && song::addAudioClip (edit, asset->second, song::getChannelID (*ch), item.track, item.start, offset, duration).isValid())
            ++report.clipsMade;
    }

    if (beyond > 0)
        report.notes.add (TRANS ("{N} clip(s) on playlist tracks after {MAX} left out").replace ("{N}", juce::String (beyond)).replace ("{MAX}", juce::String (song::numLanes)));

    if (automationItems > 0)
        report.notes.add (TRANS ("{N} automation clip(s) on the playlist left out").replace ("{N}", juce::String (automationItems)));
}

} // namespace flimport

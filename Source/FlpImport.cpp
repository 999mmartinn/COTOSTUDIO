#include "FlpImport.h"
#include "SongModel.h"
#include "ParametricEQPlugin.h"
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
        fx.isVST3 = r.count (50) > 0 && r[50].getSize() >= 4 && juce::ByteOrder::littleEndianInt (r[50].getData()) == 7;
        fx.pluginName = recordText (r, 54);
        fx.file = recordText (r, 55);
        fx.vendor = recordText (r, 56);

        if (fx.isVST3 && r.count (52) > 0 && r[52].getSize() == 16)
            fx.vst3ClassID = r[52];

        if (fx.isVST3 && r.count (53) > 0 && r[53].getSize() > 80)
        {
            auto& s = r[53];
            auto states = readRecords ((const juce::uint8*) s.getData(), s.getSize(), 80);

            if (states.count (3) > 0) fx.componentState = states[3];
            if (states.count (2) > 0) fx.controllerState = states[2];
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
    return ins.name.isNotEmpty() || ! ins.effects.isEmpty() || ! plainRouting;
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
    std::map<int, Insert> inserts;
    std::map<int, juce::Array<bool>> routing;
    const Event* params = nullptr;
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

            case 238: lastTrack = e.size >= 4 ? (int) e.u32() - 1 : -1; break;
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
                break;

            case 203: if (inEffect) fx.displayName = e.text(); break;
            case 213: if (inEffect && fx.kind == "Fruity Wrapper") readWrapper (e, fx); break;

            case 236:   // an insert's header ends; its effect slots follow
                inMixer = true;
                inEffect = false;
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

            if (isVST3 (d))
                score += 10;                                     // the VST3 version, if there is a choice

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

    const auto report = apply (*edit, p, options);
    juce::StringArray lines;
    lines.add (report.toText());
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

    auto section = [&lines] (const juce::String& title, const juce::StringArray& items)
    {
        if (items.isEmpty())
            return;

        lines.add ({});
        lines.add (title);

        for (auto& i : items)
            lines.add ("  - " + i);
    };

    section (TRANS ("Not installed on this PC (left out; scan your plugins, then import again):"), notInstalled);
    section (TRANS ("FL Studio effects COTOSTUDIO doesn't have yet (left out):"), notAvailable);
    section (TRANS ("Loaded with their default settings:"), defaultSettings);
    section (TRANS ("Left out because they were off in FL:"), leftOutOff);
    section (TRANS ("Good to know:"), notes);
    return lines.joinIntoString ("\n");
}

Report apply (te::Edit& edit, const Project& project, const Options& options)
{
    Report report;
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
                    report.notInstalled.add (where + (fx.missingInFL ? " - " + TRANS ("FL couldn't load it either")
                                                      : fx.isVST3 ? juce::String() : " - " + TRANS ("FL used its VST2 version; install the VST3 one")));
                    continue;
                }

                auto tree = te::ExternalPlugin::create (edit.engine, *desc);
                const bool canUseState = fx.isVST3 && desc->pluginFormatName == "VST3" && fx.componentState.getSize() > 0;

                if (canUseState)
                    tree.setProperty (te::IDs::state, makeVST3State (fx.componentState, fx.controllerState).toBase64Encoding(), nullptr);

                plugin = edit.getPluginCache().createNewPlugin (tree);

                if (plugin != nullptr)
                {
                    if (canUseState)
                        ++report.effectsWithSettings;
                    else if (fx.missingInFL)
                        report.defaultSettings.add (where + " - " + TRANS ("FL couldn't load it either, so no settings were saved"));
                    else
                        report.defaultSettings.add (where + (fx.isVST3 ? juce::String() : " - " + TRANS ("FL had the VST2 version, its settings don't carry over")));

                    if (fx.mix < 0.995)
                        report.notes.add (TRANS ("{WHAT}: FL mixed it in at {P}% - here it is at 100%")
                                            .replace ("{WHAT}", where).replace ("{P}", juce::String (juce::roundToInt (fx.mix * 100.0))));
                }
            }
            else if (fx.kind == "Fruity Parametric EQ 2")
            {
                plugin = song::createEffect (edit, ParametricEQPlugin::xmlTypeName, nullptr);

                if (plugin != nullptr)
                    report.defaultSettings.add (TRANS ("Parametric EQ (ours) in place of Fruity Parametric EQ 2 - set its bands again") + " (" + ins.displayName() + ")");
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
        report.notes.add (TRANS ("FL's time signature was {SIG}; COTOSTUDIO stays in 4/4")
                            .replace ("{SIG}", juce::String (project.numerator) + "/" + juce::String (project.denominator)));

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
    }

    return report;
}

} // namespace flimport

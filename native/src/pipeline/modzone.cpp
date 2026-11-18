// The mod.ff zone and its link (modzone.py; see modzone.h).
#include "modzone.h"

#include "core/parallel.h"
#include "core/pyre.h"
#include "csvtable.h"
#include "extract/cache.h"
#include "gsc/parse.h"
#include "modutil.h"
#include "textures/stage.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace w2b::pipeline
{
    using namespace mod;

    namespace mod
    {
        std::string ListRepr(const std::vector<std::string>& items)
        {
            std::string out = "[";
            for (size_t i = 0; i < items.size(); i++)
                out += (i ? ", " : "") + gsc::Repr(items[i]);
            return out + "]";
        }

        void CopyTree(const fs::path& source, const fs::path& target)
        {
            std::vector<std::pair<fs::path, fs::path>> pairs;
            fs::create_directories(target);
            for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator(); ++it)
            {
                const auto destination = target / it->path().lexically_relative(source);
                if (it->is_directory())
                    fs::create_directories(destination);
                else
                    pairs.emplace_back(it->path(), destination);
            }
            parallel::For(pairs.size(), [&](const size_t i) { Copy(pairs[i].first, pairs[i].second); }, pairs.size() < 64 ? 1 : 32);
        }

        void WriteIfChanged(const fs::path& path, const std::string& data)
        {
            std::string current;
            if (files::ReadBytes(path, current) && current == data)
                return;
            files::WriteBytes(path, data);
        }

        std::string TextBytes(const std::string_view text)
        {
            std::string out;
            for (const char c : text)
                out += c == '\n' ? std::string("\r\n") : std::string(1, c);
            return out;
        }

        void Print(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
    } // namespace mod

    namespace
    {
        const std::string TEMPLATE_MAP = "zm_test", MOD_OVERRIDE = "mod_override", TEMPLATE_SOUNDBANK = "zmb_test.all";
        const std::set<std::string> BSP_TYPES{"clipmap", "clipmap_pvs", "comworld", "gameworldsp", "gameworldmp", "mapents", "gfxworld", "skinnedverts"};
        const std::set<std::string> RUNTIME_WEAPONS{"defaultweapon_mp",
                                                    "death_self_zm",
                                                    "death_throe_zm",
                                                    "zombie_fists_zm",
                                                    "zombie_melee",
                                                    "zombie_builder_zm",
                                                    "buildable_piece_zm",
                                                    "syrette_zm",
                                                    "zombie_bowie_flourish",
                                                    "zombie_tazer_flourish",
                                                    "knife_zm",
                                                    "frag_grenade_zm",
                                                    "m1911_zm",
                                                    "m1911_upgraded_zm",
                                                    "cymbal_monkey_zm",
                                                    "claymore_zm",
                                                    "bowie_knife_zm",
                                                    "tazer_knuckles_zm",
                                                    "knife_ballistic_zm",
                                                    "knife_ballistic_upgraded_zm",
                                                    "knife_ballistic_bowie_zm",
                                                    "knife_ballistic_bowie_upgraded_zm",
                                                    "knife_ballistic_no_melee_zm",
                                                    "knife_ballistic_no_melee_upgraded_zm"};
        constexpr int MAX_DROPS = 200;

        std::string Replace(std::string text, const std::string& from, const std::string& to)
        {
            for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
                text.replace(at, from.size(), to);
            return text;
        }

        std::string StripChars(std::string_view s, std::string_view chars, bool left, bool right)
        {
            size_t a = 0, b = s.size();
            while (left && a < b && chars.find(s[a]) != std::string_view::npos)
                a++;
            while (right && b > a && chars.find(s[b - 1]) != std::string_view::npos)
                b--;
            return std::string(s.substr(a, b - a));
        }

        // files of a folder matching *.<ext> (Path.glob, one level)
        std::vector<fs::path> Glob(const fs::path& folder, const std::string& suffix)
        {
            std::vector<fs::path> found;
            std::error_code e;
            if (!fs::is_directory(folder, e))
                return found;
            for (const auto& entry : fs::directory_iterator(folder, e))
                if (EndsWith(Lower(files::Utf8(entry.path().filename())), suffix))
                    found.push_back(entry.path());
            return found;
        }

        // the text before the first "," and after it, stripped (line.split(",", 1)[1].strip())
        std::string AfterComma(const std::string& line)
        {
            const auto at = line.find(',');
            return at == std::string::npos ? std::string() : Strip(std::string_view(line).substr(at + 1));
        }

        std::string LastField(const std::string& line)
        {
            const auto parts = SplitComma(line);
            return Strip(parts.back());
        }
    } // namespace

    std::string BuildModZone(const fs::path& templateZone, std::vector<std::string>& dropped)
    {
        static const pyre::Regex templateScript("(^|/)" + TEMPLATE_MAP + "[_.]");
        std::vector<std::string> out;
        for (const auto& raw : Lines(ReadReplace(templateZone)))
        {
            const auto line = Strip(raw);
            if (StartsWith(line, ">level.ipak_read,") || StartsWith(line, ">level.ipak_write,"))
                if (EndsWith(line, "," + TEMPLATE_MAP))
                {
                    dropped.push_back(line);
                    continue;
                }
            if (!line.empty() && !StartsWith(line, "//") && !StartsWith(line, ">"))
            {
                std::vector<std::string> parts;
                for (const auto& p : SplitComma(line))
                    parts.push_back(Strip(p));
                const auto& kind = parts.front();
                const auto& name = parts.back();
                if (BSP_TYPES.count(kind) || (kind == "soundbank" && name == TEMPLATE_SOUNDBANK) || (kind == "script" && templateScript.Search(name)) ||
                    (kind == "xmodel" && name == "viewmodel_base_viewhands"))
                {
                    dropped.push_back(line);
                    continue;
                }
            }
            out.push_back(raw);
        }
        return Join(out, "\n") + "\n";
    }

    std::string OpenModIpakFirst(const std::string& zoneText, const std::vector<std::string>& extraLines)
    {
        std::optional<std::string> pack;
        for (const auto& l : extraLines)
            if (StartsWith(Strip(l), ">ipak,"))
            {
                pack = Strip(l).substr(Strip(l).find(',') + 1);
                break;
            }
        if (!pack)
            return zoneText;
        auto lines = Lines(zoneText);
        size_t header = 0;
        while (header < lines.size() && (Strip(lines[header]).empty() || StartsWith(LStrip(lines[header]), "//") || StartsWith(LStrip(lines[header]), ">")))
            header++;
        lines.insert(lines.begin() + static_cast<long long>(header), {">level.ipak_read," + *pack, ">ipak," + *pack});
        return Join(lines, "\n") + "\n";
    }

    std::string PruneTemplateWeapons(const std::string& zone, const fs::path& project, const fs::path& bo2, std::vector<std::string>& dropped)
    {
        if (!IsFile(project / "content_source/weapons.runtime.json"))
            return zone;
        std::set<std::string> needed = RUNTIME_WEAPONS;
        for (const auto& [side, ext] : {std::pair<std::string, std::string>{"maps", ".gsc"}, {"clientscripts", ".csc"}})
            for (const auto& source : files::RGlob(project / side, ext))
            {
                const auto script = gsc::Parse(Read(source), Str(source));
                const auto refs = gsc::References(script.tokens);
                std::set<size_t> definitions;
                for (const auto& [_, fn] : script.functions)
                    definitions.insert(fn.start);
                std::set<std::string> localRefs;
                for (const auto& ref : refs)
                    if (!ref.qualifier && !definitions.count(ref.index))
                        localRefs.insert(gsc::Lower(ref.name));
                std::vector<std::pair<size_t, size_t>> ignored;
                // hook_bo2_weapons replaces these template entry points; their
                // unused bodies still name every BO2 gun and are not dependencies
                if (Lower(Str(source.parent_path())) == Lower(Str(project / side / "mp")))
                    for (const std::string name : {"include_weapons", "custom_add_weapons"})
                        if (const auto* fn = script.Find(name))
                            if (!localRefs.count(name) || (side == "clientscripts" && name == "include_weapons"))
                                ignored.emplace_back(fn->start, fn->bodyClose);
                for (size_t i = 0; i < script.tokens.size(); i++)
                {
                    const auto& t = script.tokens[i];
                    if (t.kind != gsc::Kind::String)
                        continue;
                    if (std::any_of(ignored.begin(), ignored.end(), [&](const auto& r) { return i >= r.first && i <= r.second; }))
                        continue;
                    needed.insert(StripChars(StripChars(t.text, "&#", true, false), "\"", true, true));
                }
            }
        // a retained stock weapon can bring an alternate or a left-hand weapon
        std::vector<std::string> queue(needed.begin(), needed.end());
        std::set<std::string> seen;
        while (!queue.empty())
        {
            const auto name = queue.back();
            queue.pop_back();
            if (!seen.insert(name).second)
                continue;
            std::optional<fs::path> source;
            for (const auto& root : {project / "content_source", bo2 / "raw"})
            {
                for (const std::string prefix : {"", "mp", "sp"})
                {
                    const auto candidate = (prefix.empty() ? root / "weapons" : root / "weapons" / prefix) / files::FromUtf8(name);
                    if (IsFile(candidate))
                    {
                        source = candidate;
                        break;
                    }
                }
                if (source)
                    break;
            }
            if (!source)
                continue;
            const auto fields = weapons::ReadInfo(*source);
            for (const char* key : {"altWeapon", "DualWieldWeapon", "grenadeWeapon", "parentWeaponName"})
                if (fields.contains(key) && fields[key].is_string() && !fields[key].get<std::string>().empty())
                {
                    const auto dependency = fields[key].get<std::string>();
                    needed.insert(dependency);
                    queue.push_back(dependency);
                }
        }
        std::vector<std::string> output, removed;
        for (const auto& line : Lines(zone))
            if (StartsWith(Strip(line), "weapon,") && !needed.count(Basename(LastField(line))))
            {
                dropped.push_back(Strip(line));
                removed.push_back(Strip(line));
            }
            else
                output.push_back(line);
        files::WriteText(project / "content_source/template_weapons.json",
                         py::JsonDumps(py::Json{{"removed", removed}, {"required", std::vector<std::string>(needed.begin(), needed.end())}}, 2));
        // the client registration uses the same retained native set and the
        // converted weapon names as the server hooks
        const auto runtime = Json(project / "content_source/weapons.runtime.json");
        std::set<std::string> carried, native;
        const auto table = runtime.value("table", py::Json::object());
        for (const auto& w : runtime.value("weapons", py::Json::array()))
        {
            const auto name = w.get<std::string>();
            carried.insert(table.contains(name) ? table[name].get<std::string>() : name);
        }
        for (const auto& line : output)
            if (StartsWith(Strip(line), "weapon,"))
                native.insert(Basename(LastField(line)));
        std::set<std::string> both = native;
        both.insert(carried.begin(), carried.end());
        // the engine also registers weapons from this stats table: unused
        // weapon rows stay in place but inactive, like stock gaps
        const auto statsSource = bo2 / "raw/zm/zm_statstable.csv";
        if (IsFile(statsSource))
        {
            auto rows = ReadCsv(Read(statsSource));
            std::vector<std::string> inactive;
            for (size_t i = 1; i < rows.size(); i++)
            {
                auto& row = rows[i];
                if (row.size() > 4 && StartsWith(row[2], "weapon_") && !both.count(row[4] + "_zm"))
                {
                    inactive.push_back(row[0]);
                    row[2].clear();
                }
            }
            const auto destination = project / "zm/zm_statstable.csv";
            MakeParent(destination);
            files::WriteBytes(destination, CsvText(rows, "\r\n"));
            files::WriteText(project / "content_source/weapon_stats.json", py::JsonDumps(py::Json{{"inactive_weapon_rows", inactive}, {"rows", rows.size()}}, 2));
            output.push_back("stringtable,zm/zm_statstable.csv");
        }
        for (const auto& source : Glob(project / "clientscripts/mp", ".csc"))
        {
            auto script = gsc::Parse(Read(source), Str(source));
            const auto* fn = script.Find("include_weapons");
            if (!fn)
                continue;
            std::string body = "{\n";
            for (const auto& name : both)
                if (!name.empty())
                    body += "    include_weapon( \"" + name + "\", 0 );\n";
            const auto open = fn->bodyOpen, close = fn->bodyClose;
            script.tokens[open].text = body + "}";
            for (size_t i = open + 1; i <= close; i++)
                script.tokens[i].pre.clear(), script.tokens[i].text.clear();
            files::WriteText(source, gsc::Emit(script.tokens));
        }
        return Join(output, "\n") + "\n";
    }

    std::set<std::string> UnavailableWeapons(const std::vector<std::string>& unavailable)
    {
        std::set<std::string> names;
        for (const auto& entry : unavailable)
        {
            const auto head = Strip(entry.substr(0, entry.find("  <-")));
            const auto comma = head.find(',');
            const auto kind = head.substr(0, comma);
            if (kind != "weapon")
                continue;
            const auto name = Basename(comma == std::string::npos ? std::string() : head.substr(comma + 1));
            names.insert(name);
            // a weapon without its pack-a-punch variant (or vice versa) is unusable
            names.insert(name.find("_upgraded_zm") != std::string::npos ? Replace(name, "_upgraded_zm", "_zm") : Replace(name, "_zm", "_upgraded_zm"));
        }
        return names;
    }

    std::set<std::string> LoadedWeapons(const fs::path& work)
    {
        static const pyre::Regex loaded("^Loaded weapon \"([^\"]+)\"", pyre::MULTILINE);
        std::set<std::string> names;
        const auto log = Resolve(work) / "linker.log";
        if (IsFile(log))
            for (const auto& m : loaded.FindAll(ReadReplace(log)))
                names.insert(m[0]);
        return names;
    }

    std::vector<std::string> PruneWeaponReferences(const fs::path& script, const std::set<std::string>& weapons)
    {
        auto lines = Lines(ReadReplace(script));
        std::vector<std::string> pruned;
        for (auto& line : lines)
            if (std::any_of(weapons.begin(), weapons.end(), [&](const std::string& w) { return line.find("\"" + w + "\"") != std::string::npos; }) &&
                !StartsWith(LStrip(line), "//"))
            {
                pruned.push_back(Strip(line));
                line = "//waw2bo2: weapon not in mod.ff// " + line;
            }
        if (!pruned.empty())
            files::WriteText(script, Join(lines, "\n") + "\n");
        return pruned;
    }

    std::set<std::string> StockScripts(const fs::path& bo2, const fs::path& unlinker, const std::optional<fs::path>& stockDump)
    {
        static const std::set<std::string> zones{"common_zm", "patch_zm"};
        std::set<std::string> names;
        if (stockDump && IsFile(*stockDump / "catalog.json"))
        {
            const auto catalog = Json(*stockDump / "catalog.json");
            for (const auto& [filename, entry] : catalog["zones"].items())
                if (zones.count(files::Utf8(files::FromUtf8(filename).stem())))
                {
                    const auto& index = entry.contains("loaded_index") ? entry["loaded_index"] : entry["index"];
                    for (const auto& n : index.value("script", py::Json::array()))
                        names.insert(Lower(n.get<std::string>()));
                }
            return names;
        }
        for (const auto& zone : zones)
        {
            const auto ff = bo2 / "zone" / "all" / files::FromUtf8(zone + ".ff");
            const auto proc = process::RunMerged({Str(unlinker), "--no-color", "--list", "--search-path", Str(ff.parent_path()), Str(ff)});
            for (const auto& l : Lines(Text(proc.out)))
                if (StartsWith(l, "script,"))
                    names.insert(Lower(AfterComma(l)));
        }
        return names;
    }

    std::vector<std::string> ScriptClosure(const std::string& zoneText, const fs::path& raw, const std::set<std::string>& stock)
    {
        static const pyre::Regex include("^\\s*#include\\s+([\\w\\\\/]+)\\s*;", pyre::MULTILINE);
        static const pyre::Regex call("\\b([a-z_][\\w]*(?:\\\\[\\w]+)+)::", pyre::IGNORECASE);
        std::set<std::string> listed;
        for (const auto& l : Lines(zoneText))
            if (StartsWith(l, "script,"))
                listed.insert(Lower(AfterComma(l)));
        std::vector<std::string> pending(listed.begin(), listed.end()), added;
        std::set<std::string> seen = listed;
        std::error_code e;
        while (!pending.empty())
        {
            const auto name = pending.back();
            pending.pop_back();
            const auto src = raw / files::FromUtf8(name);
            if (!fs::exists(src, e))
                continue;
            const auto text = ReadReplace(src);
            const std::string ext = EndsWith(name, ".csc") ? ".csc" : ".gsc";
            std::set<std::string> refs;
            for (const auto& m : include.FindAll(text))
                refs.insert(m[0]);
            for (const auto& m : call.FindAll(text))
                refs.insert(m[0]);
            for (const auto& ref : refs)
            {
                const auto dep = Lower(Replace(ref, "\\", "/")) + ext;
                if (!seen.insert(dep).second)
                    continue;
                if (stock.count(dep) || !fs::exists(raw / files::FromUtf8(dep), e))
                    continue;
                added.push_back(dep);
                pending.push_back(dep);
            }
        }
        std::sort(added.begin(), added.end());
        return added;
    }

    std::vector<std::string> CharacterModels(const std::string& zoneText, const fs::path& raw)
    {
        static const pyre::Regex modelCall("\\b(?:precachemodel|setmodel|attach)\\s*\\(\\s*\"([^\"]+)\"", pyre::IGNORECASE);
        static const pyre::Regex aliasEntry("^\\s*\\w+\\s*\\[\\s*\\d+\\s*\\]\\s*=\\s*\"([^\"]+)\"\\s*;", pyre::MULTILINE);
        std::set<std::string> listed, found;
        std::vector<std::string> scripts;
        for (const auto& l : Lines(zoneText))
        {
            if (StartsWith(l, "xmodel,"))
                listed.insert(Lower(LastField(l)));
            if (StartsWith(l, "script,"))
                scripts.push_back(AfterComma(l));
        }
        std::error_code e;
        for (const auto& name : scripts)
        {
            const auto low = Lower(name);
            if (!(StartsWith(low, "character/") || StartsWith(low, "xmodelalias/")) || !EndsWith(name, ".gsc"))
                continue;
            const auto src = raw / files::FromUtf8(name);
            if (!fs::exists(src, e))
                continue;
            const auto text = ReadReplace(src);
            for (const auto& m : modelCall.FindAll(text))
                found.insert(m[0]);
            if (StartsWith(low, "xmodelalias/"))
                for (const auto& m : aliasEntry.FindAll(text))
                    found.insert(m[0]);
        }
        std::vector<std::string> out;
        for (const auto& m : found)
            if (!listed.count(Lower(m)) && fs::exists(raw / "xmodel" / files::FromUtf8(m + ".json"), e))
                out.push_back(m);
        return out;
    }

    namespace
    {
        // _drop_entry: (zone without the line that pulled in the failing asset, that line)
        std::pair<std::string, std::optional<std::string>> DropEntry(const std::string& zoneText, const std::string& kind, const std::string& name)
        {
            auto lines = Lines(zoneText);
            for (size_t i = 0; i < lines.size(); i++)
            {
                const auto parts = SplitComma(lines[i]);
                if (parts.size() >= 2 && Strip(parts[0]) == kind && Basename(Strip(parts.back())) == Basename(name))
                {
                    const auto line = lines[i];
                    lines.erase(lines.begin() + static_cast<long long>(i));
                    return {Join(lines, "\n") + "\n", line};
                }
            }
            return {zoneText, std::nullopt};
        }

        std::vector<std::string> RopeAssetLines(const std::optional<fs::path>& assetRoot)
        {
            if (!assetRoot || !IsFile(*assetRoot / "BSP/constraints.json"))
                return {};
            for (const auto& c : Member(*assetRoot / "BSP/constraints.json", "constraints"))
                if (c["type"] == 8)
                    return {"material,mc/rope"};
            return {};
        }

        // recover_ipak_images: the exact images the native ipak writer asked for, as IWI27
        py::Json RecoverIpakImages(const std::string& log, const std::vector<fs::path>& roots, const fs::path& output, const std::optional<fs::path>& stock,
                                   const fs::path& unlinker)
        {
            static const pyre::Regex failed("Failed to open file for ipak: images/(.+)\\.iwi");
            std::set<std::string> names;
            for (const auto& m : failed.FindAll(log))
                names.insert(m[0]);
            auto recovered = py::Json::array();
            for (const auto& name : names)
            {
                weapons::OutputName("image", name);
                const auto relative = fs::path("images") / files::FromUtf8(Replace(name, "*", "_") + ".iwi");
                const auto destination = output / relative;
                if (IsFile(destination))
                    continue;
                std::optional<fs::path> source;
                for (const auto& root : roots)
                    if (IsFile(root / relative))
                    {
                        source = root / relative;
                        break;
                    }
                if (!source)
                    for (const auto& root : roots)
                        if (IsFile(root / fs::path(relative).replace_extension(".dds")))
                        {
                            source = root / fs::path(relative).replace_extension(".dds");
                            break;
                        }
                if (!source && stock)
                    source = extract::RequiredBo2Image(*stock, name, unlinker);
                if (!source)
                    continue;
                MakeParent(destination);
                if (Lower(files::Utf8(source->extension())) == ".dds")
                    textures::ConvertFile(*source, destination);
                else
                {
                    if (Bytes(*source).substr(0, 4) != std::string("IWi\x1b", 4))
                        throw StepError("ValueError", "ipak image " + name + ": expected BO2 IWI27: " + Str(*source));
                    Copy(*source, destination);
                }
                recovered.push_back({{"image", name}, {"source", Str(*source)}, {"output", Str(destination)}});
                Print("Prepared required ipak image " + name + " from " + Str(*source));
            }
            return recovered;
        }
    } // namespace

    std::pair<fs::path, std::vector<std::string>> LinkMod(const fs::path& bo2, const fs::path& workIn, const fs::path& unlinker, std::vector<std::string> extraLines,
                                                          const std::optional<fs::path>& assetRoot, const std::vector<fs::path>& extraAssetRoots,
                                                          const std::optional<fs::path>& linkerIn, const std::optional<fs::path>& stockDump)
    {
        for (const auto& l : RopeAssetLines(assetRoot))
            extraLines.push_back(l);
        {
            std::vector<std::string> unique;
            std::set<std::string> seenLines;
            for (const auto& l : extraLines)
                if (seenLines.insert(l).second)
                    unique.push_back(l);
            extraLines = unique;
        }
        const auto work = Resolve(workIn); // the linker runs from bo2\bin
        const auto templateZone = bo2 / "mods" / files::FromUtf8(TEMPLATE_MAP) / files::FromUtf8(TEMPLATE_MAP + ".zone");
        std::vector<std::string> dropped;
        const auto zoneFile = work / "zone_source" / "mod.zone";
        fs::create_directories(zoneFile.parent_path());
        auto zoneText = OpenModIpakFirst(BuildModZone(templateZone, dropped), extraLines);
        if (assetRoot)
            zoneText = PruneTemplateWeapons(zoneText, *assetRoot, bo2, dropped);
        const auto closure = ScriptClosure(zoneText, bo2 / "raw", StockScripts(bo2, unlinker, stockDump));
        if (!closure.empty())
        {
            zoneText += "// waw2bo2: scripts the template's scripts depend on\n";
            for (const auto& s : closure)
                zoneText += "script," + s + "\n";
        }
        const auto models = CharacterModels(zoneText, bo2 / "raw");
        if (!models.empty())
        {
            zoneText += "// waw2bo2: models the template's character scripts attach at runtime\n";
            for (const auto& m : models)
                zoneText += "xmodel," + m + "\n";
        }
        if (!extraLines.empty())
        {
            zoneText += "// waw2bo2: assets the converted map needs from the mod tools' raw folder\n";
            for (const auto& l : extraLines)
                zoneText += l + "\n";
        }
        files::WriteText(work / "added_dependencies.txt", Join(closure, "\n") + "\n");
        const auto linker = linkerIn ? Resolve(*linkerIn) : bo2 / "bin" / "Linker.exe";
        std::vector<std::string> unavailable;
        const auto imageRoot = work / "ipak_images";
        auto recoveredImages = py::Json::array();
        std::set<std::string> protectedLines;
        for (const auto& line : extraLines)
            if (!Strip(line).empty() && !StartsWith(LStrip(line), "//"))
                protectedLines.insert(Strip(line));
        const auto tableOverride = work / "lobby_input";
        std::error_code e;
        fs::remove_all(tableOverride / "xanim", e);
        if (assetRoot && IsDir(*assetRoot / files::FromUtf8(MOD_OVERRIDE)))
            CopyTree(*assetRoot / files::FromUtf8(MOD_OVERRIDE), tableOverride); // stock-named assets the map replaces
        if (assetRoot)
        {
            for (const std::string name : {"mapstable.csv", "gametypestable.csv", "zm_statstable.csv"})
            {
                const auto source = *assetRoot / "zm" / name;
                if (IsFile(source))
                {
                    const auto target = tableOverride / "zm" / name;
                    MakeParent(target);
                    Copy(source, target);
                }
            }
            const auto root = Resolve(*assetRoot);
            for (const auto& line : extraLines)
            {
                if (!StartsWith(line, "rawfile,"))
                    continue;
                const auto relative = AfterComma(line);
                const auto source = Resolve(*assetRoot / files::FromUtf8(relative));
                if (Lower(Str(source)).rfind(Lower(Str(root)), 0) != 0)
                    throw StepError("ValueError", "'" + Str(source) + "' is not in the subpath of '" + Str(root) + "'");
                if (IsFile(source))
                {
                    const auto target = tableOverride / files::FromUtf8(relative);
                    MakeParent(target);
                    Copy(source, target);
                }
            }
        }
        static const pyre::Regex failedEntry("ERROR: Could not load asset \"([^\"]+)\" of type \"([^\"]+)\"");
        const auto ff = work / "out" / "mod.ff";
        bool linked = false;
        for (int attempt = 0; attempt < MAX_DROPS; attempt++)
        {
            files::WriteText(zoneFile, zoneText);
            std::vector<std::string> command{Str(linker), "--no-color", "--base-folder", Str(bo2), "--source-search-path", Str(zoneFile.parent_path()),
                                             "--add-asset-search-path", Str(bo2 / "mods" / files::FromUtf8(TEMPLATE_MAP))};
            if (assetRoot)
            {
                command.insert(command.end(), {"--add-asset-search-path", Str(Resolve(*assetRoot))});
                if (IsDir(tableOverride))
                {
                    // ?base? sorts before absolute stock paths: this override
                    // layer holds only frontend tables, so gameplay search order stays
                    command[3] = Str(tableOverride);
                    command.insert(command.end(), {"--asset-search-path", "?base?;?bin?/../raw", "--add-asset-search-path", Str(Resolve(bo2) / "raw")});
                }
            }
            for (const auto& root : extraAssetRoots)
                command.insert(command.end(), {"--add-asset-search-path", Str(Resolve(root))});
            command.insert(command.end(), {"--add-asset-search-path", Str(imageRoot), "--output-folder", Str(work / "out"), "mod"});
            const auto proc = Captured(command, Str(linker.parent_path()));
            const auto out = Text(proc.out);
            files::WriteText(work / "linker.log", out);
            // mod.ff carries no world; the map-BSP probe error is expected
            std::vector<std::string> errors;
            for (const auto& l : Lines(out))
                if (l.find("ERROR") != std::string::npos && l.find("Could not open BSP") == std::string::npos)
                    errors.push_back(l);
            if (!proc.exitCode && errors.empty() && fs::exists(ff, e))
            {
                linked = true;
                break;
            }
            std::vector<fs::path> roots;
            if (assetRoot)
                roots.push_back(*assetRoot);
            roots.insert(roots.end(), extraAssetRoots.begin(), extraAssetRoots.end());
            roots.push_back(bo2 / "mods" / files::FromUtf8(TEMPLATE_MAP));
            roots.push_back(bo2 / "raw");
            const auto recovered = RecoverIpakImages(out, roots, imageRoot, stockDump, unlinker);
            if (!recovered.empty())
            {
                for (const auto& r : recovered)
                    recoveredImages.push_back(r);
                files::WriteText(work / "ipak_images.report.json", py::JsonDumps(recoveredImages, 2) + "\n");
                continue;
            }
            const auto failed = failedEntry.FindAll(out);
            std::optional<std::string> removed;
            // the last failure is the top-level zone entry; earlier ones are its dependencies
            for (auto it = failed.rbegin(); it != failed.rend(); ++it)
            {
                auto [candidate, line] = DropEntry(zoneText, (*it)[1], (*it)[0]);
                if (line)
                {
                    removed = line;
                    if (protectedLines.count(Strip(*line)))
                        throw StepError("RuntimeError", "required converted asset cannot link: " + *line + "; it will NOT be dropped; see " + Str(work / "linker.log"));
                    zoneText = candidate;
                    auto first = errors.empty() ? std::string() : errors[0];
                    if (StartsWith(first, "ERROR: "))
                        first = first.substr(7);
                    unavailable.push_back(*line + "  <- " + first);
                    break;
                }
            }
            if (!removed)
            {
                const std::vector<std::string> head(errors.begin(), errors.begin() + static_cast<long long>(std::min<size_t>(8, errors.size())));
                throw StepError("RuntimeError", "mod.ff link failed (" + std::to_string(proc.exitCode) + "): " + ListRepr(head) + "; see " + Str(work / "linker.log"));
            }
        }
        if (!linked)
            throw StepError("RuntimeError", "mod.ff link still failing after dropping " + std::to_string(MAX_DROPS) + " template entries");
        files::WriteText(work / "dropped_from_template.txt", "# removed: belongs to the zm_test map itself\n" + Join(dropped, "\n") +
                                                                 "\n\n# removed: raw assets not available on disk\n" + Join(unavailable, "\n") + "\n");
        return {ff, unavailable};
    }
} // namespace w2b::pipeline

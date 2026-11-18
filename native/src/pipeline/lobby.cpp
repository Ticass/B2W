// The map's frontend: menu strings, lobby tables and mod_load.ff (modzone.py).
#include "modzone.h"

#include "core/hash.h"
#include "gsc/tokens.h"
#include "core/pyre.h"
#include "csvtable.h"
#include "modutil.h"
#include "weapons/weapon.h"

#include <cmath>
#include <map>

namespace w2b::pipeline
{
    using namespace mod;

    namespace
    {
        const std::string DEFAULT_MENU_MODE = "Zombies", DEFAULT_MENU_LOCATION = "World at War";

        // dict with Python's insertion order: a repeated key keeps its place
        struct Ordered
        {
            std::vector<std::pair<std::string, std::string>> items;

            void Set(const std::string& key, const std::string& value)
            {
                for (auto& [k, v] : items)
                    if (k == key)
                    {
                        v = value;
                        return;
                    }
                items.emplace_back(key, value);
            }
        };

        std::string String(const py::Json& data, const char* key)
        {
            return data.contains(key) && data[key].is_string() ? data[key].get<std::string>() : std::string();
        }

        // localization.quote
        std::string Quote(const std::string& text)
        {
            std::string out = "\"";
            for (const char c : text)
                switch (c)
                {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c;
                }
            return out + "\"";
        }

        // str(value) of a JSON scalar
        std::string PyStr(const py::Json& value)
        {
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_number_float())
                return py::FloatRepr(value.get<double>());
            if (value.is_boolean())
                return value.get<bool>() ? "True" : "False";
            return value.dump();
        }

        // mapmenu.coordinate
        void Coordinate(const py::Json& value, const std::string& label, const double limit)
        {
            double number = 0;
            bool ok = value.is_number() || value.is_boolean();
            if (value.is_number())
                number = value.get<double>();
            else if (value.is_boolean())
                number = value.get<bool>();
            else if (value.is_string())
                ok = py::ParseFloat(value.get<std::string>(), number);
            const auto l = std::to_string(static_cast<int>(limit));
            if (!ok || !std::isfinite(number) || std::abs(number) > limit)
                throw StepError("ValueError", label + " must be a number between -" + l + " and " + l + ".");
        }

        size_t CodePoints(const std::string& s)
        {
            size_t n = 0;
            for (const unsigned char c : s)
                n += (c & 0xC0) != 0x80;
            return n;
        }

        CsvRows Table(const fs::path& path)
        {
            return ReadCsv(ReadSig(path));
        }

        void WriteTable(const fs::path& path, const CsvRows& rows)
        {
            MakeParent(path);
            WriteIfChanged(path, CsvText(rows, "\n"));
        }

        bool Is(const std::vector<std::string>& row, size_t column, const std::string& value)
        {
            return row.size() > column && row[column] == value;
        }

        long long Int(const std::string& text)
        {
            long long value = 0;
            bool overflow = false;
            if (!py::ParseInt(text, value, overflow))
                throw StepError("ValueError", "invalid literal for int() with base 10: " + gsc::Repr(text));
            return value;
        }

        // rows.index(row): the first equal row
        size_t IndexOf(const CsvRows& rows, const std::vector<std::string>& row)
        {
            for (size_t i = 0; i < rows.size(); i++)
                if (rows[i] == row)
                    return i;
            throw StepError("ValueError", "list.index(x): x not in list");
        }

        std::map<std::string, std::string> MenuStrings(const fs::path& projectRoot, const std::string& project)
        {
            const auto data = MenuMetadata(projectRoot);
            const auto mode = Strip(String(data, "mode")), location = Strip(String(data, "location"));
            const auto title = String(data, "title"), description = String(data, "description");
            return {{"title", title.empty() ? project : title},
                    {"description", description.empty() ? project + ": World at War custom map" : description},
                    {"mode", mode.empty() ? DEFAULT_MENU_MODE : mode},
                    {"location", location.empty() ? DEFAULT_MENU_LOCATION : location}};
        }

        std::vector<std::string> GametypeNameKeys(const fs::path& projectRoot, const std::string& project)
        {
            const auto table = projectRoot / "zm/gametypestable.csv";
            if (!IsFile(table))
                return {};
            const auto rows = Table(table);
            std::set<std::string> modes, prefixes;
            for (const auto& r : rows)
                if (r.size() > 4 && r[0] == "6" && r[2] == project)
                    modes.insert(r[4]);
            for (const auto& r : rows)
                if (r.size() > 7 && r[0] == "0" && modes.count(r[1]) && !r[7].empty())
                    prefixes.insert(r[7]);
            std::vector<std::string> keys;
            for (const auto& prefix : prefixes)
            {
                keys.push_back(prefix + "_" + Upper(project) + "_CAPS");
                keys.push_back(prefix + "_" + MapNameKey(project) + "_LOC_CAPS");
            }
            return keys;
        }

        void StartLocationOptionStrings(const fs::path& projectRoot, const std::string& project, const std::map<std::string, std::string>& strings, Ordered& values)
        {
            const auto table = projectRoot / "zm/gametypestable.csv";
            if (!IsFile(table))
                return;
            const auto rows = Table(table);
            std::map<std::string, std::string> prefix;
            for (const auto& r : rows)
                if (r.size() > 7 && r[0] == "0" && !r[7].empty())
                    prefix[r[1]] = r[7];
            for (const auto& r : rows)
                if (r.size() > 4 && r[0] == "6" && r[2] == project)
                {
                    const auto &location = r[3], &mode = r[4];
                    values.Set(Upper("ZMUI_" + mode + "_" + project + "_CAPS"), Upper(strings.at("mode")));
                    values.Set(Upper("ZMUI_" + mode + "_" + project), strings.at("mode"));
                    if (prefix.count(mode))
                        values.Set(Upper(prefix[mode] + "_DESC_" + location), strings.at("description"));
                }
        }
    } // namespace

    py::Json MenuMetadata(const fs::path& projectRoot)
    {
        const auto path = projectRoot / "menu.json";
        if (!IsFile(path))
            return py::Json::object();
        const auto data = Json(path);
        for (const char* field : {"title", "description"})
            if (!data.contains(field) || !data[field].is_string() || Strip(data[field].get<std::string>()).empty())
                throw StepError("ValueError", std::string("menu.json requires ") + field);
        if (data.contains("icon") || data.contains("blit"))
            for (const char* field : {"icon", "blit"})
                if (!data.contains(field) || !data[field].is_string() || Strip(data[field].get<std::string>()).empty())
                    throw StepError("ValueError", std::string("menu.json requires ") + field);
        if (data.contains("longitude"))
            Coordinate(data["longitude"], "Longitude", 180);
        if (data.contains("latitude"))
            Coordinate(data["latitude"], "Latitude", 90);
        return data;
    }

    std::string MapNameKey(const std::string& project)
    {
        // mapstable column 3 is a key prefix: T6 localizes <col3>_CAPS and
        // <col3>_LOC into 32-byte buffers
        std::string key = "WAW_";
        for (const char c : Upper(project))
            key += static_cast<unsigned char>(c) >= 0x80 || py::IsWord(c) ? c : '_';
        if (CodePoints(key) + 5 > 31)
            key = "WAW_" + Upper(hash::Sha1Hex(project).substr(0, 16));
        return key;
    }

    std::vector<std::string> StageMenuAssets(const fs::path& projectRoot, const std::string& project)
    {
        const auto data = MenuMetadata(projectRoot);
        const auto strings = MenuStrings(projectRoot, project);
        const auto prefix = "WAW_MENU_" + Upper(project);
        const auto name = MapNameKey(project);
        const auto asset = "menu_" + project;
        const auto path = projectRoot / "english/localizedstrings" / files::FromUtf8(asset + ".str");
        MakeParent(path);
        Ordered values;
        values.Set(prefix + "_TITLE", strings.at("title"));
        values.Set(prefix + "_CAPS", Upper(strings.at("title")));
        values.Set(prefix + "_DESC", strings.at("description"));
        values.Set(name, strings.at("title"));
        values.Set(name + "_CAPS", Upper(strings.at("title")));
        values.Set(name + "_LOC", strings.at("location"));
        for (const auto& key : GametypeNameKeys(projectRoot, project))
            values.Set(key, Upper(strings.at("mode")));
        StartLocationOptionStrings(projectRoot, project, strings, values);
        std::string text = "VERSION \"1\"\nCONFIG \"\"\nFILENOTES \"Converted map menu\"\n\n";
        for (const auto& [key, value] : values.items)
            text += "REFERENCE " + key + "\nLANG_ENGLISH " + Quote(value) + "\n\n";
        WriteIfChanged(path, TextBytes(text + "ENDMARKER\n"));
        std::vector<std::string> assets{"localize," + asset};
        if (data.contains("loading_song") && !data["loading_song"].is_null() && !data["loading_song"].empty())
        {
            const auto bank = data["loading_song"]["bank"].get<std::string>();
            weapons::OutputName("loading sound bank", bank);
            if (!IsFile(projectRoot / "soundbank" / files::FromUtf8(bank + ".aliases.csv")))
                throw StepError("FileNotFoundError", "loading song bank missing: " + bank);
            assets.push_back("soundbank," + bank);
        }
        if (String(data, "icon").empty())
            return assets;
        std::vector<std::string> names;
        const auto add = [&](const std::string& n)
        {
            if (std::find(names.begin(), names.end(), n) == names.end())
                names.push_back(n);
        };
        for (const auto& n : {String(data, "icon"), String(data, "blit"), "menu_" + project + "_map", "menu_" + project + "_map_blur",
                              "menu_" + project + "_zclassic_default", "loadscreen_" + project + "_zclassic_default", "loadscreen_" + project + "_zclassic_"})
            add(n);
        for (const char* list : {"materials", "layer_materials"})
            for (const auto& n : data.value(list, py::Json::array()))
                add(n.get<std::string>());
        if (String(data, "layer_layout") == "aligned")
        {
            const std::string runtime = "ui_mp/t6/zombie/gamemapzombie.lua", base = "ui_mp/t6/zombie/waw_gamemap_base.lua", table = "zm/waw_menu_layers.csv";
            for (const auto& n : {base, runtime, table})
                if (!IsFile(projectRoot / files::FromUtf8(n)))
                    throw StepError("FileNotFoundError", "aligned menu runtime missing: " + n);
            assets.insert(assets.end(), {"rawfile," + base, "rawfile," + runtime, "stringtable," + table});
        }
        if (String(data, "preview_layout") == "full")
        {
            const std::string runtime = "ui/t6/mapinfoimage.lua", base = "ui/t6/waw_mapinfoimage_base.lua", table = "zm/waw_menu_preview.csv";
            for (const auto& n : {base, runtime, table})
                if (!IsFile(projectRoot / files::FromUtf8(n)))
                    throw StepError("FileNotFoundError", "full-frame lobby preview runtime missing: " + n);
            assets.insert(assets.end(), {"rawfile," + base, "rawfile," + runtime, "stringtable," + table});
        }
        const auto streamingFile = projectRoot / "images/streaming.json";
        auto streaming = IsFile(streamingFile) ? Json(streamingFile) : py::Json{{"streamingMode", py::Json::object()}};
        for (const auto& n : names)
        {
            const auto material = projectRoot / "materials" / files::FromUtf8(n + ".json");
            if (!IsFile(material))
                throw StepError("FileNotFoundError", "authored menu material missing: " + Str(material));
            for (const auto& texture : Member(material, "textures"))
            {
                const auto image = texture["image"].get<std::string>();
                if (!IsFile(projectRoot / "images" / files::FromUtf8(image + ".iwi")))
                    throw StepError("FileNotFoundError", "authored menu image missing: " + image);
                streaming["streamingMode"][image] = 2;
            }
        }
        WriteIfChanged(streamingFile, TextBytes(py::JsonDumps(streaming, 2) + "\n"));
        const auto pack = data.contains("image_pack") ? data["image_pack"].get<std::string>() : project + "_menu";
        std::vector<std::string> out{">level.ipak_read," + pack, ">ipak," + pack};
        out.insert(out.end(), assets.begin(), assets.end());
        for (const auto& n : names)
            out.push_back("material," + n);
        return out;
    }

    fs::path StageLobbyMapTable(const fs::path& stock, const fs::path& projectRoot, const std::string& project)
    {
        // GameGlobeZombie reads numeric longitude/latitude from columns 16/17;
        // an unknown map crashes the return to the lobby
        auto rows = Table(stock);
        if (rows.empty() || rows[0].size() < 20)
            throw StepError("ValueError", "unsupported zombies map table schema");
        const auto find = [&](const std::string& key) -> std::optional<size_t>
        {
            for (size_t i = 0; i < rows.size(); i++)
                if (!rows[i].empty() && rows[i][0] == key)
                    return i;
            return std::nullopt;
        };
        auto existing = find(project);
        if (!existing)
        {
            const auto defaults = find("default");
            if (!defaults)
                throw StepError("ValueError", "zombies map table lacks default metadata");
            auto row = rows[*defaults];
            if (row.size() < 20)
                row.resize(20);
            row[0] = project;
            row[3] = project;
            row[19] = "top";
            rows.insert(rows.begin() + static_cast<long long>(*defaults), row);
            existing = *defaults;
        }
        if (rows[*existing].size() < 20)
            rows[*existing].resize(20);
        // the frontend Lua reads every column of the map's row: empty ones
        // take the base-game map's values (content index 0: no DLC needed)
        std::optional<size_t> base;
        for (size_t i = 1; i < rows.size() && !base; i++)
        {
            const auto& r = rows[i];
            if (!r.empty() && r[0] != "maxnum_map" && r[0] != "default" && r[0] != project && r.size() > 11 && Strip(r[11]) == "0")
                base = i;
        }
        if (!base)
            throw StepError("ValueError", "zombies map table lacks a base-game map row");
        auto& row = rows[*existing];
        for (size_t column = 1; column < 20; column++)
            if (column != 3 && column != 5 && column != 16 && column != 17 && column != 18 && Strip(row[column]).empty())
                row[column] = rows[*base].at(column);
        for (const size_t column : {16, 17, 18})
        {
            if (Strip(row[column]).empty())
                row[column] = "0";
            double value = 0;
            if (!py::ParseFloat(row[column], value))
                throw StepError("ValueError", "invalid globe coordinate for " + project);
            if (!std::isfinite(value))
                throw StepError("ValueError", "nonfinite globe coordinate for " + project);
        }
        CsvRows maps;
        for (size_t i = 1; i < rows.size(); i++)
            if (!rows[i].empty() && rows[i][0] != "maxnum_map" && rows[i][0] != "default")
                maps.push_back(rows[i]);
        const auto count = find("maxnum_map");
        if (!count)
            throw StepError("ValueError", "zombies map table lacks map count");
        rows[*count].at(1) = std::to_string(maps.size());
        if (Strip(row[5]).empty())
            row[5] = std::to_string(IndexOf(maps, row));
        // strings come from stage_menu_assets' localize asset, for every map
        row[3] = MapNameKey(project);
        const auto metadata = MenuMetadata(projectRoot);
        if (metadata.contains("icon"))
            row[4] = PyStr(metadata["icon"]);
        for (const auto& [column, field] : {std::pair<size_t, const char*>{16, "longitude"}, {17, "latitude"}})
            if (metadata.contains(field))
                row[column] = PyStr(metadata[field]);
        row[6] = "WAW_MENU_" + Upper(project) + "_DESC";
        const auto result = projectRoot / "zm/mapstable.csv";
        WriteTable(result, rows);
        return result;
    }

    fs::path StageLobbyGametypeTable(const fs::path& stock, const fs::path& projectRoot, const std::string& project)
    {
        // one complete WaW play area with the stock four-player co-op mode
        auto rows = Table(stock);
        if (rows.empty() || rows[0].size() < 23)
            throw StepError("ValueError", "unsupported zombies gametype table schema");
        // category 5 describes selectable locations; 6 connects them to modes
        for (const auto& [category, countKey] : {std::pair<std::string, std::string>{"5", "maxnum_startloc"}, {"6", "startloc_gamemode_map"}})
        {
            const auto entries = [&]
            {
                std::vector<size_t> out;
                for (size_t i = 0; i < rows.size(); i++)
                    if (!rows[i].empty() && rows[i][0] == category)
                        out.push_back(i);
                return out;
            };
            const auto countRow = [&]() -> std::optional<size_t>
            {
                for (size_t i = 0; i < rows.size(); i++)
                    if (!rows[i].empty() && rows[i][0] == countKey)
                        return i;
                return std::nullopt;
            };
            const auto nextIndex = [&]
            {
                long long top = -1;
                for (const auto i : entries())
                    top = std::max(top, Int(rows[i].at(1)));
                return std::to_string(top + 1);
            };
            if (!countRow())
                throw StepError("ValueError", "zombies gametype table lacks " + countKey);
            const auto list = entries();
            if (!std::any_of(list.begin(), list.end(), [&](const size_t i) { return Is(rows[i], 2, project); }))
            {
                const auto index = nextIndex();
                std::vector<std::string> entry;
                if (category == "5")
                    entry = {"5", index, project, "default", project, project, "menu_zm_map_zombie_dot", "1", "0", "0", "180", "-50", "", "", "", "", project, "100", "-90", "", "", "", ""};
                else
                {
                    entry = {"6", index, project, "default", "zclassic", "0", "left", "YES"};
                    entry.resize(23);
                }
                const auto at = list.empty() ? IndexOf(rows, rows[*countRow()]) + 1 : IndexOf(rows, rows[list.back()]) + 1;
                rows.insert(rows.begin() + static_cast<long long>(at), entry);
            }
            // engine iteration uses these bounds, not the physical CSV row count
            rows[*countRow()].at(1) = nextIndex();
        }
        size_t entryAt = rows.size();
        for (size_t i = 0; i < rows.size() && entryAt == rows.size(); i++)
            if (rows[i].size() > 2 && rows[i][0] == "5" && rows[i][2] == project)
                entryAt = i;
        if (entryAt == rows.size())
            throw StepError("StopIteration", "");
        // SelectStartLocZombie.lua reads a location's columns by category and
        // location name, first row wins: the map's row goes ahead of the
        // stock test maps that share location "default"
        auto entry = rows[entryAt];
        rows.erase(rows.begin() + static_cast<long long>(IndexOf(rows, entry)));
        size_t first = 0;
        while (first < rows.size() && !(!rows[first].empty() && rows[first][0] == "5"))
            first++;
        rows.insert(rows.begin() + static_cast<long long>(first), entry);
        // file order alone did not win in game: the unlisted test maps get
        // their own location name, so the map holds the only row with its location
        std::set<std::string> clashing;
        for (const auto& r : rows)
            if (r.size() > 3 && r[0] == "5" && r[3] == entry[3] && r[2] != project)
                clashing.insert(r[2]);
        for (auto& r : rows)
            if (r.size() > 3 && (r[0] == "5" || r[0] == "6") && clashing.count(r[2]) && r[3] == entry[3])
                r[3] = r[2];
        auto& e = rows[first];
        const auto metadata = MenuMetadata(projectRoot);
        if (metadata.contains("start_location") && !metadata["start_location"].is_null() && !metadata["start_location"].empty())
        {
            // aligned artwork: selection on the Blit's object, Nuketown's
            // offsets for the option list (+180, -50) and the badge (+100, -90)
            std::vector<long long> v;
            for (const auto& c : metadata["start_location"])
                v.push_back(c.is_number() ? static_cast<long long>(c.get<double>()) : Int(PyStr(c)));
            if (v.size() != 2)
                throw StepError("ValueError", "too many values to unpack");
            const auto x = v[0], y = v[1];
            e.at(8) = std::to_string(x), e.at(9) = std::to_string(y), e.at(10) = std::to_string(x + 180), e.at(11) = std::to_string(y - 50);
            e.at(17) = std::to_string(x + 100), e.at(18) = std::to_string(y - 90);
        }
        const auto prefix = "WAW_MENU_" + Upper(project);
        const auto blit = metadata.contains("blit") ? PyStr(metadata["blit"]) : e.at(6);
        e.at(4) = prefix + "_CAPS", e.at(5) = prefix + "_DESC", e.at(6) = blit, e.at(16) = prefix + "_TITLE";
        const auto result = projectRoot / "zm/gametypestable.csv";
        WriteTable(result, rows);
        return result;
    }

    fs::path LinkLobby(const fs::path& bo2, const fs::path& projectRoot, const std::string& project, const fs::path& work, const fs::path& linker,
                       const std::optional<fs::path>& stock)
    {
        // frontend-only mod_load.ff, so selection works before loading a map
        StageLobbyMapTable(bo2 / "raw/zm/mapstable.csv", projectRoot, project);
        StageLobbyGametypeTable(bo2 / "raw/zm/gametypestable.csv", projectRoot, project);
        const auto root = Resolve(work) / "lobby";
        const auto materials = root / "assets/materials";
        fs::create_directories(materials);
        const auto frame = bo2 / "raw/materials/menu_zm_map_frame.json";
        const std::vector<std::string> names{"menu_" + project + "_map", "menu_" + project + "_map_blur", "menu_" + project + "_zclassic_default",
                                             "loadscreen_" + project + "_zclassic_default", "loadscreen_" + project + "_zclassic_"};
        // mod.ff already links the loading song's bank: a second copy drops
        std::vector<std::string> authored;
        for (const auto& entry : StageMenuAssets(projectRoot, project))
            if (!StartsWith(entry, "soundbank,"))
                authored.push_back(entry);
        const auto metadata = MenuMetadata(projectRoot);
        for (const auto& name : names)
            if (!String(metadata, "icon").empty())
            {
                if (!IsFile(projectRoot / "materials" / files::FromUtf8(name + ".json")))
                    throw StepError("FileNotFoundError", "authored menu backdrop missing: " + name);
            }
            else
                Copy(frame, materials / files::FromUtf8(name + ".json"));
        const auto source = root / "zone_source";
        fs::create_directories(source);
        std::vector<std::string> lines;
        std::set<std::string> seen;
        for (const auto& entry : authored)
            if (seen.insert(entry).second)
                lines.push_back(entry);
        for (const auto& name : names)
            if (seen.insert("material," + name).second)
                lines.push_back("material," + name);
        std::string zone = ">game,T6\nstringtable,zm/mapstable.csv\nstringtable,zm/gametypestable.csv\ntechniqueset,,trivial_9z33feqw\nimage,,menu_zm_map_frame\n";
        for (const auto& l : lines)
            zone += l + "\n";
        files::WriteText(source / "mod_load.zone", zone);
        const auto out = Resolve(work) / "out";
        std::vector<std::string> command{Str(Resolve(linker)), "--no-color", "--base-folder", Str(Resolve(projectRoot)), "--source-search-path", Str(source),
                                         "--asset-search-path", "?base?", "--add-asset-search-path", Str(root / "assets"), "--add-asset-search-path",
                                         Str(Resolve(bo2) / "raw")};
        if (stock)
            command.insert(command.end(), {"--add-asset-search-path", Str(Resolve(*stock))});
        command.insert(command.end(), {"--output-folder", Str(out), "mod_load"});
        const auto proc = Captured(command);
        const auto log = Text(proc.out);
        files::WriteText(root / "linker.log", log);
        const auto result = out / "mod_load.ff";
        static const pyre::Regex failure("(?m)^(?:ERROR:|Missing asset|Failed to load|Could not load)");
        if (proc.exitCode || !IsFile(result) || failure.Search(log))
            throw StepError("RuntimeError", "lobby zone link failed; see " + Str(root / "linker.log"));
        std::error_code e;
        if (!metadata.empty())
            files::WriteText(out / "menu_metadata.json", py::JsonDumps(metadata, 2) + "\n");
        else
            fs::remove(out / "menu_metadata.json", e);
        return result;
    }

    void VerifyLobbyTables(const fs::path& ff, const fs::path& unlinker, const fs::path& work, const std::string& project)
    {
        // stock search-path precedence must not have lost our registrations
        const auto stem = files::Utf8(ff.stem());
        const auto dump = Resolve(work) / files::FromUtf8("verify_" + stem + "_lobby");
        std::error_code e;
        fs::remove_all(dump, e);
        const auto proc = Captured({Str(Resolve(unlinker)), "--no-color", "--include-assets", "stringtable", "--output-folder", Str(dump), Str(Resolve(ff))});
        files::WriteText(Resolve(work) / files::FromUtf8("verify_" + stem + "_lobby.log"), Text(proc.out));
        if (proc.exitCode)
            throw StepError("RuntimeError", "cannot verify lobby tables in " + Str(ff));
        for (const std::string name : {"mapstable.csv", "gametypestable.csv"})
        {
            const auto path = dump / "zm" / name;
            if (!IsFile(path))
                throw StepError("RuntimeError", "missing lobby table " + name + " in " + Str(ff));
            auto bytes = Bytes(path);
            if (bytes.rfind("\xEF\xBB\xBF", 0) == 0)
                bytes.erase(0, 3);
            const auto rows = ReadCsv(bytes);
            bool valid;
            if (name == "mapstable.csv")
                valid = std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return !r.empty() && r[0] == project; });
            else
            {
                static const std::string categories[] = {"5", "6"};
                valid = std::all_of(std::begin(categories), std::end(categories), [&](const std::string& category)
                                    {
                                        return std::any_of(rows.begin(), rows.end(), [&](const auto& r)
                                                           { return r.size() > 7 && r[0] == category && r[2] == project && r[3] == "default" &&
                                                                    (category == "5" || (r[4] == "zclassic" && r[7] == "YES")); });
                                    });
            }
            if (!valid)
                throw StepError("RuntimeError", "custom lobby registration missing from " + name + " in " + Str(ff));
        }
    }
} // namespace w2b::pipeline

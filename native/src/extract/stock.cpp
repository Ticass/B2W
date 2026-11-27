// Game caches (see stock.h).
#include "stock.h"
#include "extract/unlinker.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "shaders/techsets.h"
#include "core/regex.h"

#include <algorithm>
#include <cstdio>
#include <set>

#ifndef W2B_VERSION
#define W2B_VERSION "0.0.0"
#endif
#ifndef W2B_BUILD_LABEL
#define W2B_BUILD_LABEL ""
#endif

namespace w2b::extract
{
    namespace
    {
        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        std::string Posix(const fs::path& p)
        {
            return files::Utf8(p.generic_wstring());
        }

        std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        void Print(const std::string& line)
        {
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }

        bool SamePath(const fs::path& a, const fs::path& b)
        {
            return Lower(Str(a)) == Lower(Str(b));
        }

        void SortPaths(std::vector<fs::path>& paths)
        {
            std::stable_sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b)
                             {
                                 std::vector<std::string> ka, kb;
                                 for (const auto& p : a)
                                     ka.push_back(Lower(files::Utf8(p)));
                                 for (const auto& p : b)
                                     kb.push_back(Lower(files::Utf8(p)));
                                 return ka < kb;
                             });
        }

        std::vector<fs::path> RGlobAll(const fs::path& folder)
        {
            std::vector<fs::path> found;
            std::error_code e;
            if (!fs::is_directory(folder, e))
                return found;
            for (fs::recursive_directory_iterator it(folder, e), end; !e && it != end; it.increment(e))
                if (it->is_regular_file(e))
                    found.push_back(it->path());
            return found;
        }

        std::vector<fs::path> GlobExtension(const fs::path& folder, const std::string& extension)
        {
            std::vector<fs::path> found;
            std::error_code e;
            for (fs::directory_iterator it(folder, e), end; !e && it != end; it.increment(e))
                if (Lower(files::Utf8(it->path().extension())) == extension)
                    found.push_back(it->path());
            return found;
        }

        bool Within(const fs::path& path, const fs::path& folder)
        {
            const auto relative = path.lexically_relative(folder);
            return !relative.empty() && *relative.begin() != "..";
        }

        std::string LStripComma(const std::string& text)
        {
            const auto first = text.find_first_not_of(',');
            return first == std::string::npos ? "" : text.substr(first);
        }

        // keep only the basic wooden barrier's render closure from prison
        util::Json TrimBarrierPayload(const fs::path& folder, util::Json data)
        {
            const std::string asset = "zmcore_basicwoodbarrier";
            const auto barrier = folder / "zbarrier" / asset;
            if (!IsFile(barrier))
                throw Error("RuntimeError", "Prison donor is missing required barrier " + asset);
            std::set<std::string> keep{Posix(barrier.lexically_relative(folder))};
            std::vector<std::string> parts;
            {
                const auto text = files::ReadText(barrier);
                size_t start = 0;
                for (size_t i = 0; i <= text.size(); i++)
                    if (i == text.size() || text[i] == '\\')
                    {
                        parts.push_back(text.substr(start, i - start));
                        start = i + 1;
                    }
                parts.erase(parts.begin());
            }
            static const regex::Regex modelField(R"(model\d+$)", regex::IGNORECASE);
            std::set<std::string> models;
            for (size_t i = 0; i + 1 < parts.size(); i += 2)
                if (!parts[i + 1].empty() && modelField.Search(parts[i]))
                    models.insert(LStripComma(parts[i + 1]));
            std::set<std::string> materials;
            const auto root = Resolve(folder);
            for (const auto& name : models)
            {
                const auto relative = "xmodel/" + name + ".json";
                const auto path = folder / files::FromUtf8(relative);
                if (!IsFile(path))
                    throw Error("RuntimeError", "Barrier dependency model " + name + " is missing from prison donor");
                keep.insert(relative);
                for (const auto& lod : ReadJson(path).value("lods", util::Json::array()))
                {
                    const auto file = lod["file"].get<std::string>();
                    keep.insert(file);
                    const auto gltf = ReadJson(folder / files::FromUtf8(file));
                    for (const auto& m : gltf.value("materials", util::Json::array()))
                        if (m.contains("name") && m["name"].is_string() && !m["name"].get<std::string>().empty())
                            materials.insert(LStripComma(m["name"].get<std::string>()));
                    std::vector<util::Json> uris;
                    for (const auto& b : gltf.value("buffers", util::Json::array()))
                        uris.push_back(b);
                    for (const auto& b : gltf.value("images", util::Json::array()))
                        uris.push_back(b);
                    for (const auto& item : uris)
                    {
                        const auto uri = item.value("uri", std::string());
                        if (uri.empty() || uri.starts_with("data:"))
                            continue;
                        const auto dependency = Resolve((folder / files::FromUtf8(file)).parent_path() / files::FromUtf8(uri));
                        if (!Within(dependency, root))
                            throw Error("RuntimeError", "Barrier GLTF dependency escapes the cache");
                        keep.insert(Posix(dependency.lexically_relative(root)));
                    }
                }
            }
            for (const auto& name : materials)
            {
                const auto path = folder / assets::MaterialPath(name);
                if (!IsFile(path))
                    throw Error("RuntimeError", "Barrier material " + name + " is missing from prison donor");
                const auto material = ReadJson(path);
                keep.insert(Posix(path.lexically_relative(folder)));
                if (const auto technique = material.value("techniqueSet", std::string()); !technique.empty())
                {
                    const auto techniquePath = folder / "techniquesets" / files::FromUtf8(technique + ".json");
                    if (IsFile(techniquePath))
                    {
                        keep.insert(Posix(techniquePath.lexically_relative(folder)));
                        const auto shaders = shaders::ProgramFiles(shaders::ReadT6TechniqueSet(techniquePath));
                        keep.insert(shaders.begin(), shaders.end());
                    }
                }
                for (const auto& texture : material.value("textures", util::Json::array()))
                {
                    const auto image = LStripComma(texture.value("image", std::string()));
                    if (!image.empty() && image.front() != '$')
                        keep.insert(Posix(assets::ImagePath(image, ".iwi")));
                }
            }
            const std::set<std::string> payloads{"xmodel", "model_export", "images", "zbarrier", "materials", "techniquesets", "shader_bin"};
            util::Json kept = util::Json::array();
            for (const auto& n : data["files"])
            {
                const auto name = n.get<std::string>();
                if (payloads.count(name.substr(0, name.find('/'))) && !keep.count(name))
                    fs::remove(folder / files::FromUtf8(name));
                else
                    kept.push_back(name);
            }
            const auto before = data["files"].size();
            data["files"] = kept;
            WriteJson(folder / "extraction.json", data);
            Print("Extract All: prison barrier closure retained (" + std::to_string(models.size()) + " models, " + std::to_string(kept.size()) + "/" + std::to_string(before)
                  + " files)");
            return data;
        }

        int Rank(const std::string& name)
        {
            const auto slash = name.find_last_of('/');
            auto stem = slash == std::string::npos ? name : name.substr(slash + 1);
            if (const auto dot = stem.find_last_of('.'); dot != std::string::npos && dot)
                stem = stem.substr(0, dot);
            const char* core[] = {"patch_zm", "code_post_gfx_zm", "common_zm", "code_post_gfx", "common"};
            for (int i = 0; i < 5; i++)
                if (stem == core[i])
                    return i;
            return 5;
        }

        void RequireWorldDumps(const fs::path& ff, const fs::path& folder, const util::Json& data, const fs::path& tool)
        {
            for (const auto& name : data["index"].value("gfxworld", util::Json::array()))
                if (!IsFile(folder / files::FromUtf8("waw2bo2/" + name.get<std::string>() + ".gfx.bin")))
                    throw Error("RuntimeError", files::Utf8(ff.filename()) + ": the WaW extractor did not export map world " + name.get<std::string>() + " (waw2bo2/"
                                                    + name.get<std::string>() + ".gfx.bin); extractor: " + Str(tool));
        }
    } // namespace

    void RequireWawExtractor(const fs::path& tool)
    {
        if (!IsFile(tool))
            throw Error("RuntimeError", "Missing: " + files::Utf8(tool.filename()));
        std::string bytes;
        files::ReadBytes(tool, bytes);
        if (bytes.find("waw2bo2_primary_lights") == std::string::npos)
            throw Error("RuntimeError", Str(tool) + " is not the converter's WaW extractor: it cannot export map worlds. "
                                                    "Clear \"WaW extractor folder\" in Setup > Advanced Paths and click Auto-detect to use the bundled one.");
    }

    std::string StockAssets(const std::string& engine, const fs::path& ff)
    {
        const auto stem = files::Utf8(ff.stem());
        if (engine == "T4")
            return stem == "code_post_gfx" ? "rawfile,image,lightdef" : "rawfile";
        std::string assets = "material,techniqueset,snddriverglobals,zbarrier";
        if (stem == "zm_prison")
            assets += ",image,xmodel";
        return assets;
    }

    std::vector<fs::path> StockZones(const fs::path& game, const std::string& engine)
    {
        std::vector<fs::path> zones;
        for (const auto& p : RGlobAll(game / "zone"))
        {
            if (Lower(files::Utf8(p.extension())) != ".ff")
                continue;
            if (engine == "T6")
            {
                static const std::set<std::string> selected{"zm_nuked", "common_zm", "code_pre_gfx_zm", "code_post_gfx_zm", "patch_zm", "zm_prison"};
                if (!selected.count(files::Utf8(p.stem())) || files::Utf8(p.parent_path().filename()) != "all")
                    continue;
            }
            zones.push_back(p);
        }
        std::sort(zones.begin(), zones.end(), [&](const fs::path& a, const fs::path& b)
                  {
                      const auto na = Posix(a.lexically_relative(game)), nb = Posix(b.lexically_relative(game));
                      return std::make_pair(Rank(na), na) < std::make_pair(Rank(nb), nb);
                  });
        return zones;
    }

    util::Json Inventory(const fs::path& game, const fs::path& tool, const std::string& engine)
    {
        util::Json archives = util::Json::object();
        for (const char* directory : {"main", "zone", "sound"})
            for (const auto& p : RGlobAll(game / directory))
            {
                const auto ext = Lower(files::Utf8(p.extension()));
                if (ext == ".iwd" || ext == ".ipak" || ext == ".sabl" || ext == ".sabs")
                    archives[Str(p.lexically_relative(game))] = Stamp(p);
            }
        util::Json derived = util::Json::object();
        if (engine == "T6")
        {
            std::vector<fs::path> paths{game / "t6zm.exe"};
            for (const char* ext : {".gsc", ".csc"})
            {
                auto found = files::GlobSuffix(game / "raw", ext);
                SortPaths(found);
                paths.insert(paths.end(), found.begin(), found.end());
            }
            for (const auto& p : paths)
                if (IsFile(p))
                    derived[Str(p.lexically_relative(game))] = Stamp(p);
        }
        util::Json zones = util::Json::object();
        for (const auto& p : StockZones(game, engine))
            zones[Posix(p.lexically_relative(game))] = Stamp(p);
        util::Json out = util::Json::object();
        out["schema"] = SCHEMA;
        out["engine"] = engine;
        out["game"] = Str(Resolve(game));
        out["tool"] = UnlinkerIdentity(tool);
        out["archives"] = archives;
        out["derived"] = derived;
        out["zones"] = zones;
        return util::SortKeys(out);
    }

    namespace
    {
        // Removes the zone extractions under <root>/dependencies and
        // <root>/custom_maps that another unlinker made: their cache keys
        // include the unlinker, so nothing finds them again.
        void PruneOtherUnlinkerGenerations(const fs::path& root, const std::string& identity)
        {
            std::error_code e;
            const auto prune = [&](const fs::path& zoneFolder)
            {
                std::vector<fs::path> entries;
                for (fs::directory_iterator it(zoneFolder, e), end; !e && it != end; it.increment(e))
                    entries.push_back(it->path());
                for (const auto& generation : entries)
                    try
                    {
                        if (IsFile(generation / "extraction.json") && ReadJson(generation / "extraction.json").at("inputs").at("tool") != identity)
                            fs::remove_all(generation, e);
                    }
                    catch (const std::exception&)
                    {
                    }
                // pointers (<key>.json) to generations that are gone
                for (const auto& pointer : entries)
                    try
                    {
                        if (Lower(files::Utf8(pointer.extension())) == ".json" &&
                            !fs::is_directory(zoneFolder / files::FromUtf8(ReadJson(pointer).at("folder").get<std::string>()), e))
                            fs::remove(pointer, e);
                    }
                    catch (const std::exception&)
                    {
                    }
            };
            const auto subfolders = [&](const fs::path& folder)
            {
                std::vector<fs::path> found;
                for (fs::directory_iterator it(folder, e), end; !e && it != end; it.increment(e))
                    if (it->is_directory(e))
                        found.push_back(it->path());
                return found;
            };
            for (const auto& zone : subfolders(root / "dependencies"))
                prune(zone);
            for (const auto& map : subfolders(root / "custom_maps"))
                for (const auto& zone : subfolders(map))
                    prune(zone);
        }
    } // namespace

    fs::path Ready(const fs::path& game, const fs::path& tool, const fs::path& root, const std::string& engine)
    {
        try
        {
            const auto receipt = ReadJson(root / "all2raw.json");
            const auto raw = files::FromUtf8(receipt.at("raw").get<std::string>());
            if (util::SortKeys(receipt.at("inventory")) == Inventory(game, tool, engine) && IsFile(raw / "catalog.json"))
            {
                const auto catalog = ReadJson(raw / "catalog.json");
                bool complete = true;
                for (const auto& [name, _] : catalog.at("owners").items())
                    complete = complete && IsFile(raw / files::FromUtf8(name));
                for (const auto& [_, entry] : receipt.at("zones").items())
                    for (const auto& name : entry.at("files"))
                        complete = complete && IsFile(files::FromUtf8(entry["folder"].get<std::string>()) / files::FromUtf8(name.get<std::string>()));
                if (complete && (engine != "T6" || IsFile(raw / "t6api_cache.json")))
                    return raw;
            }
        }
        catch (const std::exception&)
        {
        }
        throw Error("RuntimeError", engine + " game asset cache is missing or stale. Run Extract All in Setup before building.");
    }

    void BuildT6ApiCache(const fs::path& snapshotPath, const fs::path& cache, const bool withAssets, const fs::path& stockDump)
    {
        if (IsFile(cache))
            try
            {
                const auto data = ReadJson(cache);
                const auto assets = data.value("stock_assets", util::Json::object());
                if (data.value("version", 0) == 4 && (!withAssets || !assets.empty()))
                    return;
            }
            catch (const std::exception&)
            {
            }
        const auto snapshot = ReadJson(snapshotPath);
        util::Json assets = util::Json::object();
        if (withAssets && IsFile(stockDump / "catalog.json"))
        {
            const std::set<std::string> stockZones{"code_pre_gfx_zm", "code_post_gfx_zm", "common_zm", "patch_zm"};
            std::vector<std::string> order;
            std::map<std::string, std::set<std::string>> sets;
            const auto catalog = ReadJson(stockDump / "catalog.json");
            for (const auto& [filename, entry] : catalog["zones"].items())
            {
                if (!stockZones.count(files::Utf8(files::FromUtf8(filename).stem())))
                    continue;
                const auto& index = entry.contains("loaded_index") ? entry["loaded_index"] : entry["index"];
                for (const auto& [kind, names] : index.items())
                {
                    if (!sets.count(kind))
                        order.push_back(kind);
                    for (const auto& n : names)
                        sets[kind].insert(Lower(n.get<std::string>()));
                }
            }
            for (const auto& kind : order)
                assets[kind] = std::vector<std::string>(sets[kind].begin(), sets[kind].end());
        }
        util::Json data = util::Json::object();
        data["version"] = 4;
        data["builtins"] = snapshot["builtins"];
        data["methods"] = snapshot["methods"];
        data["scripts"] = snapshot["scripts"];
        data["stock_assets"] = assets;
        fs::create_directories(cache.parent_path());
        files::WriteBytes(cache, util::JsonDumpsDefault(data));
    }

    fs::path Prepare(const fs::path& game, const fs::path& tool, const fs::path& root, const std::string& engine, const bool refresh, std::optional<int> workers)
    {
        if (!refresh)
            try
            {
                const auto raw = Ready(game, tool, root, engine);
                Print("Extract All: reusing complete " + engine + " cache: " + Str(raw));
                const CacheLock lock(root);
                PruneOtherUnlinkerGenerations(root, UnlinkerIdentity(tool));
                return raw;
            }
            catch (const Error&)
            {
            }
        const auto zones = StockZones(game, engine);
        if (zones.empty())
            throw Error("FileNotFoundError", "No installed fastfiles found in " + Str(game / "zone"));
        if (engine == "T4")
            RequireWawExtractor(tool);
        const auto inputs = Inventory(game, tool, engine);
        const auto toolHash = inputs["tool"].get<std::string>();
        CacheLock lock(root);
        const auto outputs = ParallelZones(
            zones,
            [&](const fs::path& ff)
            {
                const auto name = Posix(ff.lexically_relative(game));
                const auto zoneRoot = root / "zones" / files::FromUtf8(Key(util::Json(name)));
                Print("Extract All: " + engine + " starting " + name);
                ZoneRequest request{ff, tool, zoneRoot, SearchPaths({ff.parent_path(), game / "zone/all", game / "main", game / "sound", game}), StockAssets(engine, ff),
                                    engine == "T4" ? "DDS" : "IWI", toolHash, inputs["archives"], refresh};
                auto [folder, data] = ExtractZone(request);
                if (engine == "T6" && files::Utf8(ff.stem()) == "zm_prison")
                    data = TrimBarrierPayload(folder, data);
                util::Json entry = util::Json::object();
                entry["folder"] = Str(folder);
                entry["files"] = data["files"];
                entry["index"] = data["index"];
                entry["loaded_index"] = data["loaded_index"];
                entry["inputs"] = data["inputs"];
                return entry;
            },
            engine, workers);
        util::Json entries = util::Json::object();
        for (size_t i = 0; i < zones.size(); i++)
            entries[Posix(zones[i].lexically_relative(game))] = outputs[i];
        const auto generation = Key(util::Json{{"entries", entries}, {"inventory", inputs}});
        auto raw = root / "views" / files::FromUtf8(generation);
        if (IsFile(raw / "catalog.json"))
        {
            bool complete = true;
            const auto catalog = ReadJson(raw / "catalog.json");
            for (const auto& [name, _] : catalog["owners"].items())
                complete = complete && IsFile(raw / files::FromUtf8(name));
            if (!complete)
                raw = root / "views" / files::FromUtf8(generation + "-" + Key(util::Json(Str(raw) + std::to_string(std::rand()))).substr(0, 8));
        }
        if (!IsFile(raw / "catalog.json"))
        {
            const auto pending = root / "views" / files::FromUtf8("pending-" + Key(util::Json(generation + std::to_string(std::rand()))));
            fs::create_directories(pending);
            util::Json owners = util::Json::object();
            util::Json conflicts = util::Json::array();
            size_t number = 0;
            for (const auto& [name, entry] : entries.items())
            {
                Print("Extract All: assembling " + engine + " cache " + std::to_string(++number) + "/" + std::to_string(entries.size()) + " " + name + " ("
                      + std::to_string(entry["files"].size()) + " files)");
                const auto folder = files::FromUtf8(entry["folder"].get<std::string>());
                for (const auto& f : entry["files"])
                {
                    const auto filename = f.get<std::string>();
                    if (filename.starts_with("zone_source/"))
                        continue;
                    const auto source = folder / files::FromUtf8(filename);
                    const auto target = pending / files::FromUtf8(filename);
                    if (owners.contains(filename))
                    {
                        std::error_code e;
                        if (fs::file_size(target, e) != fs::file_size(source, e) || Digest(target) != Digest(source))
                            conflicts.push_back(util::Json{{"asset", filename}, {"selected", owners[filename]}, {"alternative", name}});
                        continue;
                    }
                    std::error_code e;
                    fs::create_directories(target.parent_path(), e);
                    fs::create_hard_link(source, target, e);
                    if (e)
                        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
                    owners[filename] = name;
                }
            }
            WriteJson(pending / "catalog.json", util::Json{{"schema", SCHEMA}, {"zones", entries}, {"owners", owners}, {"conflicts", conflicts}});
            Publish(pending, raw);
        }
        if (engine == "T6")
        {
            Print("Extract All: indexing BO2 script/API metadata");
            BuildT6ApiCache(ResourceRoot() / "src/waw2bo2/compat/t6_script_api.json", raw / "t6api_cache.json", true, raw);
        }
        WriteJson(root / "all2raw.json", util::Json{{"schema", SCHEMA}, {"engine", engine}, {"raw", Str(raw)}, {"zones", entries}, {"inventory", inputs}});
        // prune obsolete stock generations and partial views
        Print("Extract All: removing obsolete stock exports and partial views");
        std::set<std::wstring> keep;
        for (const auto& [_, entry] : entries.items())
            keep.insert(Resolve(files::FromUtf8(entry["folder"].get<std::string>())).wstring());
        std::error_code e;
        for (fs::directory_iterator zoneIt(root / "zones", e), end; !e && zoneIt != end; zoneIt.increment(e))
        {
            if (!zoneIt->is_directory(e))
                continue;
            std::vector<fs::path> generations;
            for (fs::directory_iterator it(zoneIt->path(), e), stop; !e && it != stop; it.increment(e))
                generations.push_back(it->path());
            for (const auto& g : generations)
            {
                if (fs::is_directory(g, e) && !keep.count(Resolve(g).wstring()))
                    fs::remove_all(g, e);
                else if (Lower(files::Utf8(g.extension())) == ".json")
                    try
                    {
                        const auto target = zoneIt->path() / files::FromUtf8(ReadJson(g).at("folder").get<std::string>());
                        if (!keep.count(Resolve(target).wstring()))
                            fs::remove(g, e);
                    }
                    catch (const std::exception&)
                    {
                    }
            }
        }
        std::vector<fs::path> views;
        for (fs::directory_iterator it(root / "views", e), end; !e && it != end; it.increment(e))
            views.push_back(it->path());
        for (const auto& v : views)
            if (fs::is_directory(v, e) && Resolve(v) != Resolve(raw))
                fs::remove_all(v, e);
        PruneOtherUnlinkerGenerations(root, UnlinkerIdentity(tool));
        Print("Extract All: " + engine + " cache ready (" + std::to_string(zones.size()) + " zones): " + Str(raw));
        return raw;
    }

    std::vector<std::pair<std::string, fs::path>> SourceDumps(const MapSettings& settings, const fs::path& wawCache, const bool refresh)
    {
        const auto source = Resolve(settings.fastfile);
        const auto folder = source.parent_path();
        RequireWawExtractor(settings.t4Tool);
        const auto root = wawCache / "custom_maps" / files::FromUtf8(Key(util::Json(Str(folder))));
        util::Json dependencies = util::Json::object();
        for (const auto& directory : {folder, settings.waw / "main"})
            for (const auto& p : GlobExtension(directory, ".iwd"))
                dependencies[Str(p)] = Stamp(p);
        CacheLock lock(root);
        const auto stem = files::Utf8(source.stem());
        std::vector<std::pair<std::string, fs::path>> selected{
            {"map", source}, {"patch", folder / files::FromUtf8(stem + "_patch.ff")}, {"mod", folder / "mod.ff"}};
        auto extras = GlobExtension(folder, ".ff");
        SortPaths(extras);
        if (Within(folder, Resolve(settings.waw / "zone")))
        {
            const std::set<std::string> companions{stem + "_load", "localized_" + stem, "localized_" + stem + "_patch"};
            std::erase_if(extras, [&](const fs::path& ff) { return !companions.count(files::Utf8(ff.stem())); });
        }
        const auto expected = selected;
        for (const auto& ff : extras)
            if (std::none_of(expected.begin(), expected.end(), [&](const auto& s) { return SamePath(s.second, ff); }))
                selected.emplace_back("extra:" + files::Utf8(ff.stem()), ff);
        std::erase_if(selected, [](const auto& s) { return !IsFile(s.second); });
        std::vector<fs::path> unique;
        for (const auto& [_, ff] : selected)
            if (std::none_of(unique.begin(), unique.end(), [&](const fs::path& u) { return SamePath(u, ff); }))
                unique.push_back(ff);
        // the map's own zones are dumped again by every converter build: a dump an older build cached is
        // never reused (a stale Purple Dimension dump held the stock font atlas the map replaces). Stock
        // zones stay keyed on the tool alone: they are large and the same for every map.
        const auto toolHash = UnlinkerIdentity(settings.t4Tool) + ":short-staging:" + W2B_VERSION + ":" + W2B_BUILD_LABEL;
        const auto outputs = ParallelZones(
            unique,
            [&](const fs::path& ff)
            {
                Print("Extract All: custom map starting " + files::Utf8(ff.filename()));
                ZoneRequest request{ff, settings.t4Tool, root / files::FromUtf8(Key(util::Json(Str(ff)))), SearchPaths({folder, settings.waw / "main"}), std::string(WAW_ASSETS),
                                    "DDS", toolHash, dependencies, refresh};
                const auto [output, data] = ExtractZone(request);
                RequireWorldDumps(ff, output, data, settings.t4Tool);
                return util::Json(Str(output));
            },
            "custom map");
        std::vector<std::pair<std::string, fs::path>> result;
        for (const auto& [role, ff] : selected)
            for (size_t i = 0; i < unique.size(); i++)
                if (SamePath(unique[i], ff))
                {
                    result.emplace_back(role, files::FromUtf8(outputs[i].get<std::string>()));
                    break;
                }
        return result;
    }
} // namespace w2b::extract

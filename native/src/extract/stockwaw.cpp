// Stock WaW zones (see stockwaw.h).
#include "stockwaw.h"
#include "extract/unlinker.h"

#include "cache.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/process.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace w2b::extract
{
    namespace
    {
        const std::string DUMP_ASSETS = "material,techniqueset,image,fx,xmodel,weapon,xanim,sound,loadedsound,rawfile,comworld,physpreset,snddriverglobals";
        const std::string DUMP_MARKER = ".waw2bo2_dump_v6_source_techniques";

        std::string Lower(std::string s)
        {
            for (auto& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return s;
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        int Rank(const std::string& zone)
        {
            return zone.rfind("nazi_zombie", 0) == 0 ? 0 : zone.rfind("localized", 0) == 0 ? 2 : 1;
        }

        std::set<std::string> SplitComma(const std::string& text)
        {
            std::set<std::string> out;
            for (const auto part : py::Split(text, ","))
                out.emplace(part);
            return out;
        }
    } // namespace

    StockWaw::StockWaw(fs::path wawRoot_, fs::path unlinker_, fs::path work_) : wawRoot(std::move(wawRoot_)), unlinker(std::move(unlinker_)), work(std::move(work_))
    {
    }

    std::optional<StockWaw> StockWaw::FromJson(const py::Json& data)
    {
        if (data.is_null())
            return std::nullopt;
        StockWaw stock(files::FromUtf8(data["waw_root"].get<std::string>()), files::FromUtf8(data["unlinker"].get<std::string>()),
                       files::FromUtf8(data["work"].get<std::string>()));
        stock.Load();
        if (data.contains("prepared"))
            stock.ApplyPrepared(data["prepared"]);
        return stock;
    }

    py::Json StockWaw::PreparedJson() const
    {
        py::Json out = py::Json::object();
        for (const auto& [zone, folder] : prepared)
            out[zone] = py::Json{{"folder", Str(folder)}, {"inputs", preparedSources.count(zone) ? preparedSources.at(zone) : py::Json::object()}};
        return out;
    }

    void StockWaw::ApplyPrepared(const py::Json& data)
    {
        for (const auto& [zone, entry] : data.items())
        {
            prepared[zone] = files::FromUtf8(entry["folder"].get<std::string>());
            preparedSources[zone] = entry["inputs"];
        }
    }

    fs::path StockWaw::ZoneDir() const
    {
        return wawRoot / "zone" / "english";
    }

    void StockWaw::Load()
    {
        lookup.clear();
        const auto shared = work / "all2raw.json";
        if (IsFile(shared))
        {
            const auto data = ReadJson(shared);
            for (const auto& [filename, entry] : data["zones"].items())
            {
                const auto path = files::FromUtf8(filename);
                if (Lower(files::Utf8(path.parent_path().filename())) == "english")
                {
                    const auto zone = files::Utf8(path.stem());
                    index[zone] = entry["index"];
                    prepared[zone] = files::FromUtf8(entry["folder"].get<std::string>());
                    preparedSources[zone] = entry["inputs"];
                }
            }
            return;
        }
        const auto cache = work / "stock_index.json";
        if (IsFile(cache))
        {
            const auto data = ReadJson(cache);
            if (data.value("version", 0) == 1)
            {
                index = data["zones"];
                return;
            }
        }
        std::vector<fs::path> zones;
        std::error_code e;
        for (const auto& entry : fs::directory_iterator(ZoneDir(), e))
            if (Lower(files::Utf8(entry.path().extension())) == ".ff")
                zones.push_back(entry.path());
        std::sort(zones.begin(), zones.end(), [](const fs::path& a, const fs::path& b) { return Lower(Str(a)) < Lower(Str(b)); });
        py::Json out = py::Json::object();
        for (const auto& ff : zones)
        {
            const auto result = RunUnlinker({Str(unlinker), "--no-color", "--list", Str(ff)});
            py::Json types = py::Json::object();
            const auto listing = encoding::Utf8Replace(result.out);
            for (const auto line : py::SplitLines(listing))
            {
                const auto comma = line.find(',');
                const auto kind = std::string(py::Strip(line.substr(0, comma)));
                const auto name = comma == std::string_view::npos ? std::string() : std::string(py::Strip(line.substr(comma + 1)));
                if (name.empty() || name[0] == ',') // a reference, not a definition
                    continue;
                if (!types.contains(kind))
                    types[kind] = py::Json::array();
                types[kind].push_back(name);
            }
            out[files::Utf8(ff.stem())] = types;
        }
        fs::create_directories(work);
        files::WriteText(cache, py::JsonDumpsDefault(py::Json{{"version", 1}, {"zones", out}}));
        index = out;
    }

    std::vector<std::string> StockWaw::ZonesDefining(const std::string& kind, const std::string& name) const
    {
        if (lookup.empty())
            for (const auto& [zone, types] : index.items())
                for (const auto& [k, names] : types.items())
                    for (const auto& n : names)
                    {
                        auto& hits = lookup[{k, Lower(n.get<std::string>())}];
                        if (hits.empty() || hits.back() != zone)
                            hits.push_back(zone);
                    }
        const auto it = lookup.find({kind, Lower(name)});
        if (it == lookup.end())
            return {};
        auto zones = it->second;
        std::sort(zones.begin(), zones.end(), [](const std::string& a, const std::string& b) { return std::pair(Rank(a), a) < std::pair(Rank(b), b); });
        return zones;
    }

    fs::path StockWaw::Dump(const std::string& zone, const std::optional<std::string>& kind)
    {
        if (prepared.count(zone))
        {
            const auto inputs = preparedSources.count(zone) ? preparedSources[zone] : py::Json::object();
            const bool unknown = !inputs.contains("assets") || inputs["assets"].is_null();
            const auto available = SplitComma(unknown ? std::string() : inputs["assets"].get<std::string>());
            const auto requested = SplitComma(kind.value_or(DUMP_ASSETS));
            if (unknown || std::includes(available.begin(), available.end(), requested.begin(), requested.end()))
                return prepared[zone];
            // recover only the requested asset types, retaining those already recovered
            std::set<std::string> all = available;
            all.insert(requested.begin(), requested.end());
            all.erase("");
            std::string assets;
            for (const auto& a : all)
                assets += (assets.empty() ? "" : ",") + a;
            const auto ff = files::FromUtf8(inputs["file"].get<std::string>());
            const auto root = work / "dependencies" / Key(py::Json(Str(ff)));
            CacheLock lock(root);
            std::fprintf(stderr, "Extracting referenced WaW stock zone: %s (%s)\n", files::Utf8(ff.filename()).c_str(), assets.c_str());
            ZoneRequest request{ff, unlinker, root, SearchPaths({ff.parent_path(), wawRoot / "main", wawRoot}), assets, "DDS"};
            request.dependencies = inputs.value("dependencies", py::Json());
            request.base = prepared[zone];
            const auto extraction = ExtractZone(request);
            prepared[zone] = extraction.folder;
            preparedSources[zone] = extraction.data["inputs"];
            return extraction.folder;
        }
        const auto out = work / files::FromUtf8(zone);
        if (!fs::exists(out / DUMP_MARKER))
        {
            const auto log = work / files::FromUtf8(zone + ".log");
            const auto result = RunUnlinker({Str(unlinker), "--no-color", "--search-path", Str(wawRoot / "main"), "--image-format", "DDS", "--model-format", "GLTF",
                                                    "--include-assets", DUMP_ASSETS, "--output-folder", Str(out), Str(ZoneDir() / files::FromUtf8(zone + ".ff"))});
            fs::create_directories(log.parent_path());
            files::WriteBytes(log, result.out);
            if (result.exitCode)
                throw std::runtime_error("dump of stock WaW zone " + zone + " failed (" + std::to_string(result.exitCode) + "); see " + Str(log));
            files::WriteBytes(out / DUMP_MARKER, "");
        }
        return out;
    }

    std::optional<std::pair<std::string, fs::path>> StockWaw::RootFor(const std::string& kind, const std::string& name)
    {
        const auto zones = ZonesDefining(kind, name);
        if (zones.empty())
            return std::nullopt;
        return std::pair{zones[0], Dump(zones[0], kind)};
    }
} // namespace w2b::extract

// Diagnostics bundle (see diagnostics.h).
#include "diagnostics.h"

#include "build.h"
#include "core/encoding.h"
#include "core/files.h"
#include "core/hash.h"
#include "core/regex.h"
#include "core/zipwrite.h"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <set>

#ifndef W2B_VERSION
#define W2B_VERSION "0.0.0"
#endif

namespace w2b::launcher
{
    namespace
    {
        bool Within(const fs::path& path, const fs::path& folder)
        {
            const auto relative = path.lexically_relative(folder);
            return !relative.empty() && *relative.begin() != "..";
        }

        std::string Platform()
        {
            OSVERSIONINFOW info{sizeof info};
            using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
            if (const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
                get(&info);
            return "Windows-" + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." + std::to_string(info.dwBuildNumber);
        }

        // sha256 of the whole file, or of its first and last MiB when it is larger (zones are hundreds of MiB)
        util::Json Fingerprint(const fs::path& path, const std::uintmax_t size)
        {
            constexpr std::uintmax_t chunk = 1 << 20;
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                return {{"read_error", "cannot open"}};
            std::string data;
            const bool sampled = size > 2 * chunk;
            if (!sampled)
            {
                data.resize(static_cast<size_t>(size));
                stream.read(data.data(), static_cast<std::streamsize>(size));
            }
            else
            {
                data.resize(static_cast<size_t>(2 * chunk));
                stream.read(data.data(), chunk);
                stream.seekg(static_cast<std::streamoff>(size - chunk));
                stream.read(data.data() + chunk, chunk);
            }
            return {{"sha256", hash::Sha256Hex(data)}, {"sampled", sampled}};
        }

        // what the converter read and the game loads: names, sizes, dates and fingerprints, never the files
        util::Json ListFiles(const fs::path& folder, const bool recursive, const std::set<std::string>& extensions = {}, const size_t limit = 3000)
        {
            util::Json out = util::Json::array();
            std::error_code ec;
            if (folder.empty() || !fs::is_directory(folder, ec))
                return out;
            auto add = [&](const fs::directory_entry& entry) {
                if (out.size() >= limit || !entry.is_regular_file(ec))
                    return;
                if (!extensions.empty() && !extensions.contains(Lower(Utf8(entry.path().extension()))))
                    return;
                const auto size = entry.file_size(ec);
                util::Json row{{"path", files::Utf8(entry.path().lexically_relative(folder).generic_wstring())}, {"size", size}};
                if (const auto time = entry.last_write_time(ec); !ec)
                    row["modified"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(time).time_since_epoch()).count();
                row.update(Fingerprint(entry.path(), size));
                out.push_back(std::move(row));
            };
            if (recursive)
                for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                    add(*it);
            else
                for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                    add(*it);
            return out;
        }

        // the tail of a file, at most limit bytes
        std::string Tail(const fs::path& path, const long long limit)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                return {};
            const auto size = static_cast<long long>(stream.tellg());
            const auto from = std::max<long long>(0, size - limit);
            std::string data(static_cast<size_t>(size - from), '\0');
            stream.seekg(from);
            stream.read(data.data(), static_cast<std::streamsize>(data.size()));
            return data;
        }
    } // namespace

    const char* Version()
    {
        return W2B_VERSION;
    }

    fs::path CreateBundle(const Settings& settings, const std::string& console, const std::string& error)
    {
        std::error_code ec;
        const auto work = fs::weakly_canonical(fs::absolute(Path(settings.work)), ec);
        const auto cache = work / "asset_cache";
        std::optional<fs::path> build;
        try
        {
            build = BuildPaths::ForSettings(settings).root;
        }
        catch (const std::exception&)
        {
        }
        std::set<fs::path> found;
        if (build && fs::exists(*build, ec))
            for (fs::recursive_directory_iterator it(*build, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const auto name = Lower(Utf8(it->path().filename()));
                if (name.ends_with(".log") || name.ends_with("report.json") || name == "weapons.dependencies.json" || name == "weapons.stage.json"
                    || name == "bone_hitbox_audit.json")
                    found.insert(it->path());
            }
        if (fs::is_regular_file(cache / "extract.log", ec))
            found.insert(cache / "extract.log");
        std::string logs = console;
        if (build && fs::is_regular_file(*build / "build.log", ec))
        {
            std::string bytes;
            files::ReadBytes(*build / "build.log", bytes);
            logs += "\n" + encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
        }
        static const regex::Regex logRef(R"(\blog: (.*?)(?=\[tool\]|\r|\n|$))");
        for (const auto& row : logRef.FindAll(logs))
        {
            auto value = row[0];
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            const auto path = fs::weakly_canonical(fs::absolute(Path(value)), ec);
            if (Lower(Utf8(path.extension())) == ".log" && (Within(path, cache) || (build && Within(path, *build))))
                found.insert(path);
        }
        const auto directory = work / "diagnostics";
        fs::create_directories(directory);
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto destination = directory / Path((settings.project.empty() ? std::string("extraction") : settings.project) + (error.empty() ? "-diagnostics-" : "-failure-") +
                                                  std::to_string(stamp) + ".zip");
        util::Json manifest = util::Json::array();
        zip::Writer archive(destination);
        util::Json summary = util::Json::object();
        summary["version"] = Version();
        summary["platform"] = Platform();
        summary["error"] = error;
        summary["settings"] = settings.ToJson();
        archive.Add("summary.json", util::JsonDumps(summary, 2));
        archive.Add("console.log", console);
        for (const auto& path : found)
        {
            std::string name;
            if (build && Within(path, *build))
                name = "build/" + files::Utf8(path.lexically_relative(*build).generic_wstring());
            else if (Within(path, cache))
                name = "asset_cache/" + files::Utf8(path.lexically_relative(cache).generic_wstring());
            else
                continue;
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                manifest.push_back({{"file", Utf8(path)}, {"read_error", "cannot open"}});
                continue;
            }
            const auto size = static_cast<long long>(stream.tellg());
            const auto from = std::max<long long>(0, size - 2 * 1024 * 1024);
            std::string data(static_cast<size_t>(size - from), '\0');
            stream.seekg(from);
            stream.read(data.data(), static_cast<std::streamsize>(data.size()));
            archive.Add(name, data);
            manifest.push_back({{"file", name}, {"original_bytes", size}, {"included_bytes", data.size()}});
        }
        auto include = [&](const fs::path& path, const std::string& name, const long long limit) {
            if (!fs::is_regular_file(path, ec))
                return;
            const auto data = Tail(path, limit);
            archive.Add(name, data);
            manifest.push_back({{"file", name}, {"original_bytes", static_cast<long long>(fs::file_size(path, ec))}, {"included_bytes", data.size()}});
        };
        const auto pluto = PlutoniumRoot();
        const auto installed = settings.project.empty() ? fs::path() : pluto / "storage/t6/mods" / Path(settings.project);
        if (build && !settings.project.empty())
        {
            const auto zoneRaw = *build / "stage/zone_raw" / Path(settings.project);
            // every staging warning and error in full (the report itself can be larger than a log tail)
            try
            {
                const auto report = util::Json::parse(files::ReadText(zoneRaw / "bridge_stage.report.json"));
                std::string text;
                for (const auto* key : {"errors", "warnings"})
                {
                    text += std::string(key) + ":\n";
                    for (const auto& item : report.value(key, util::Json::array()))
                        text += (item.is_string() ? item.get<std::string>() : item.dump()) + "\n";
                    text += "\n";
                }
                archive.Add("warnings.txt", text);
            }
            catch (const std::exception& reportError)
            {
                manifest.push_back({{"file", "warnings.txt"}, {"read_error", reportError.what()}});
            }
            // what the build made of the map's menus, HUD, fonts and strings
            for (const auto& folder : {zoneRaw / "content_source", zoneRaw / "zm", zoneRaw / "ui"})
                for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
                {
                    const auto name = Lower(Utf8(it->path().filename()));
                    if (it->is_regular_file(ec) && (name.find("menu") != std::string::npos || name.find("hud") != std::string::npos ||
                                                     name.find("font") != std::string::npos || name.find("locali") != std::string::npos))
                        include(it->path(), "build/" + files::Utf8(it->path().lexically_relative(*build).generic_wstring()), 4 * 1024 * 1024);
                }
        }
        // the game's own log of the last match (written while the game runs with logfile set; Play sets it)
        for (const auto* log : {"console_zm.log", "console_zm.log.old"})
            include(installed / log, std::string("game/") + log, 4 * 1024 * 1024);
        // the WaW files the converter read and the mod the game loads: the cause when two PCs build the same code differently
        const auto waw = Path(settings.waw), tools = Path(settings.waw_tools), map = Path(settings.fastfile);
        util::Json environment{{"paths", {{"waw", settings.waw}, {"waw_tools", settings.waw_tools}, {"fastfile", settings.fastfile}, {"plutonium", Utf8(pluto)}}}};
        environment["waw_zone"] = ListFiles(waw / "zone", true, {".ff"});
        environment["waw_main"] = ListFiles(waw / "main", true, {".iwd", ".ff", ".cfg"});
        environment["waw_raw_ui"] = ListFiles(waw / "raw/ui", true);
        if (fs::weakly_canonical(tools, ec) != fs::weakly_canonical(waw, ec))
            environment["waw_tools_raw_ui"] = ListFiles(tools / "raw/ui", true);
        if (!map.empty())
            environment["map_folder"] = ListFiles(map.parent_path(), true, {".ff", ".iwd", ".arena", ".csv"});
        environment["installed_mod"] = ListFiles(installed, false);
        archive.Add("environment.json", util::JsonDumps(environment, 2));
        // the menu sources the pause menu is translated from: small text, needed when they fail to parse
        for (const auto& root : {tools / "raw", waw / "raw"})
        {
            const auto ui = root / "ui";
            if (!fs::is_directory(ui, ec))
                continue;
            for (fs::recursive_directory_iterator it(ui, ec), end; !ec && it != end; it.increment(ec))
            {
                const auto extension = Lower(Utf8(it->path().extension()));
                if (it->is_regular_file(ec) && (extension == ".txt" || extension == ".menu" || extension == ".inc") && it->file_size(ec) <= 256 * 1024)
                    include(it->path(), "raw_ui/" + files::Utf8(it->path().lexically_relative(ui).generic_wstring()), 256 * 1024);
            }
            break; // the folder the converter reads first
        }
        archive.Add("manifest.json", util::JsonDumps(manifest, 2));
        archive.Close();
        return destination;
    }
} // namespace w2b::launcher

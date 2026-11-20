// Diagnostics bundle (see diagnostics.h).
#include "diagnostics.h"

#include "build.h"
#include "core/encoding.h"
#include "core/files.h"
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
        const auto destination = directory / Path((settings.project.empty() ? std::string("extraction") : settings.project) + "-failure-" + std::to_string(stamp) + ".zip");
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
        archive.Add("manifest.json", util::JsonDumps(manifest, 2));
        archive.Close();
        return destination;
    }
} // namespace w2b::launcher

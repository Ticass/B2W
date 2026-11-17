// Port of launcher.py Settings / discover / naming (see settings.h).
#include "settings.h"

#include "core/files.h"
#include "core/pyre.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace w2b::launcher
{
    namespace
    {
        bool EndsWith(const std::string& text, const std::string& suffix)
        {
            return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        struct Field
        {
            const char* name;
            std::string Settings::* text;
            bool Settings::* flag;
        };

        // asdict order
        const std::vector<Field>& Fields()
        {
            static const std::vector<Field> fields{
                {"waw", &Settings::waw, nullptr},
                {"bo2", &Settings::bo2, nullptr},
                {"waw_tools", &Settings::waw_tools, nullptr},
                {"t4", &Settings::t4, nullptr},
                {"t6", &Settings::t6, nullptr},
                {"decoder", &Settings::decoder, nullptr},
                {"work", &Settings::work, nullptr},
                {"fastfile", &Settings::fastfile, nullptr},
                {"project", &Settings::project, nullptr},
                {"menu_title", &Settings::menu_title, nullptr},
                {"menu_description", &Settings::menu_description, nullptr},
                {"menu_mode", &Settings::menu_mode, nullptr},
                {"menu_location", &Settings::menu_location, nullptr},
                {"menu_blit", &Settings::menu_blit, nullptr},
                {"menu_large", &Settings::menu_large, nullptr},
                {"menu_blur", &Settings::menu_blur, nullptr},
                {"menu_loading", &Settings::menu_loading, nullptr},
                {"menu_icon", &Settings::menu_icon, nullptr},
                {"menu_location_point", &Settings::menu_location_point, nullptr},
                {"menu_preview", &Settings::menu_preview, nullptr},
                {"menu_longitude", &Settings::menu_longitude, nullptr},
                {"menu_latitude", &Settings::menu_latitude, nullptr},
                {"loading_song", &Settings::loading_song, nullptr},
                {"source_fx", nullptr, &Settings::source_fx},
                {"bo2_stock_perks", nullptr, &Settings::bo2_stock_perks},
                {"remaster", nullptr, &Settings::remaster},
                {"redump", nullptr, &Settings::redump},
                {"resume", nullptr, &Settings::resume},
                {"verbose", nullptr, &Settings::verbose},
            };
            return fields;
        }

        // the bundled tool folders discover() fills and save() clears again
        const std::pair<const char*, const char*> BUNDLED[] = {{"t4", "vendor/OpenAssetTools/build/bin/Release_x86"},
                                                               {"t6", "vendor/OpenAssetToolsT6/build/bin/Release_x86"},
                                                               {"decoder", "tools/bin/xaudio_wma_decoder.exe"}};

        std::wstring Env(const wchar_t* name)
        {
            const wchar_t* value = _wgetenv(name);
            return value ? value : L"";
        }

        bool IsDir(const fs::path& path)
        {
            std::error_code error;
            return fs::is_directory(path, error);
        }

        bool IsFile(const fs::path& path)
        {
            std::error_code error;
            return fs::is_regular_file(path, error);
        }

        fs::path Resolve(const fs::path& path)
        {
            std::error_code error;
            const auto resolved = fs::weakly_canonical(fs::absolute(path, error), error);
            return error ? fs::absolute(path) : resolved;
        }
    } // namespace

    std::string Lower(std::string text)
    {
        for (auto& c : text)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return text;
    }

    std::string Utf8(const fs::path& path)
    {
        // str(Path): Windows separators, as pathlib writes them
        auto preferred = path;
        return files::Utf8(preferred.make_preferred());
    }

    fs::path Path(const std::string& utf8)
    {
        return files::FromUtf8(utf8);
    }

    fs::path UserDirectory()
    {
        const auto local = Env(L"LOCALAPPDATA");
        return (local.empty() ? fs::path(Env(L"USERPROFILE")) : fs::path(local)) / L"WawConverter";
    }

    fs::path ResourceRoot()
    {
        if (const auto explicitRoot = Env(L"W2B_RESOURCE_ROOT"); !explicitRoot.empty())
            return explicitRoot;
        wchar_t buffer[MAX_PATH * 4];
        const auto length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        fs::path folder = fs::path(std::wstring(buffer, length)).parent_path();
        // the checkout (native/build/...) or the portable folder beside the exe
        for (auto probe = folder; !probe.empty(); probe = probe.parent_path())
        {
            if (IsDir(probe / "vendor") || IsDir(probe / "tools" / "bin"))
                return probe;
            if (probe == probe.root_path())
                break;
        }
        return folder;
    }

    Settings::Settings() : work(Utf8(UserDirectory() / "builds")) {}

    const std::vector<std::string>& Settings::TextFields()
    {
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> out;
            for (const auto& f : Fields())
                if (f.text)
                    out.push_back(f.name);
            return out;
        }();
        return names;
    }

    const std::vector<std::string>& Settings::FlagFields()
    {
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> out;
            for (const auto& f : Fields())
                if (f.flag)
                    out.push_back(f.name);
            return out;
        }();
        return names;
    }

    std::string* Settings::Text(const std::string& name)
    {
        for (const auto& f : Fields())
            if (f.text && name == f.name)
                return &(this->*f.text);
        return nullptr;
    }

    bool* Settings::Flag(const std::string& name)
    {
        for (const auto& f : Fields())
            if (f.flag && name == f.name)
                return &(this->*f.flag);
        return nullptr;
    }

    py::Json Settings::ToJson() const
    {
        py::Json data = py::Json::object();
        for (const auto& f : Fields())
        {
            if (f.text)
                data[f.name] = this->*f.text;
            else
                data[f.name] = this->*f.flag;
        }
        return data;
    }

    Settings Settings::FromJson(const py::Json& data)
    {
        Settings settings;
        if (!data.is_object())
            return settings;
        for (const auto& f : Fields())
        {
            const auto it = data.find(f.name);
            if (it == data.end())
                continue;
            if (f.text && it->is_string())
                settings.*f.text = it->get<std::string>();
            else if (f.flag && it->is_boolean())
                settings.*f.flag = it->get<bool>();
        }
        return settings;
    }

    Settings Settings::Load(const std::optional<fs::path>& path)
    {
        try
        {
            return FromJson(py::Json::parse(files::ReadText(path.value_or(UserDirectory() / "settings.json"))));
        }
        catch (const std::exception&)
        {
            return Settings();
        }
    }

    void Settings::Save(const std::optional<fs::path>& path) const
    {
        const auto target = path.value_or(UserDirectory() / "settings.json");
        auto data = ToJson();
        const auto root = ResourceRoot();
        for (const auto& [key, relative] : BUNDLED)
        {
            const auto value = data[key].get<std::string>();
            if (!value.empty() && Resolve(Path(value)) == Resolve(root / relative))
                data[key] = "";
        }
        auto temp = target;
        temp.replace_extension(".tmp");
        files::WriteText(temp, py::JsonDumps(data, 2));
        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("cannot save " + Utf8(target));
    }

    Settings Discover(Settings settings)
    {
        const auto root = ResourceRoot();
        std::vector<fs::path> steamPaths;
        {
            HKEY key;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS)
            {
                wchar_t value[MAX_PATH * 2];
                DWORD size = sizeof value, type = 0;
                if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS && type == REG_SZ)
                    steamPaths.emplace_back(std::wstring(value));
                RegCloseKey(key);
            }
        }
        const auto programs = Env(L"ProgramFiles(x86)");
        steamPaths.push_back(fs::path(programs.empty() ? L"C:/Program Files (x86)" : programs) / "Steam");
        std::vector<fs::path> libraries = steamPaths;
        static const pyre::Regex library(R"re("path"\s+"([^"]+)")re");
        for (const auto& steam : steamPaths)
        {
            try
            {
                const auto text = files::ReadText(steam / "steamapps/libraryfolders.vdf");
                for (const auto& row : library.FindAll(text))
                {
                    std::string value = row[0];
                    for (size_t at = value.find("\\\\"); at != std::string::npos; at = value.find("\\\\", at + 1))
                        value.replace(at, 2, "\\");
                    libraries.push_back(Path(value));
                }
            }
            catch (const std::exception&)
            {
            }
        }
        for (const auto& [field, folder] : {std::pair<std::string Settings::*, const char*>{&Settings::waw, "Call of Duty World at War"},
                                            {&Settings::bo2, "Call of Duty Black Ops II"}})
        {
            if (!(settings.*field).empty())
                continue;
            for (const auto& p : libraries)
                if (IsDir(p / "steamapps/common" / folder))
                {
                    settings.*field = Utf8(p / "steamapps/common" / folder);
                    break;
                }
        }
        for (const auto& [key, relative] : BUNDLED)
            if (auto* value = settings.Text(key); value->empty())
                *value = Utf8(root / relative);
        if (settings.waw_tools.empty())
        {
            std::vector<fs::path> candidates{root / "wawModTools"};
            if (!settings.waw.empty())
                candidates.push_back(Path(settings.waw));
            for (const auto& p : candidates)
                if (IsFile(p / "bin/linker_pc.exe"))
                {
                    settings.waw_tools = Utf8(p);
                    break;
                }
        }
        return settings;
    }

    fs::path MapZone(const fs::path& source)
    {
        const auto stem = Utf8(source.stem());
        if (EndsWith(Lower(stem), "_load"))
        {
            auto sibling = source.parent_path() / Path(stem.substr(0, stem.size() - 5) + Utf8(source.extension()));
            if (IsFile(sibling))
                return sibling;
        }
        return source;
    }

    std::vector<fs::path> MapFastfiles(const fs::path& folder)
    {
        std::vector<fs::path> found;
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        {
            const auto& p = it->path();
            if (Lower(Utf8(p.extension())) != ".ff")
                continue;
            const auto stem = Lower(Utf8(p.stem()));
            if (stem == "mod" || stem == "mod_load" || stem == "common" || stem == "code_post_gfx" || EndsWith(stem, "_patch") || MapZone(p) != p)
                continue;
            found.push_back(p);
        }
        std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) { return Lower(Utf8(a.filename())) < Lower(Utf8(b.filename())); });
        return found;
    }

    std::string ProjectName(const std::string& source)
    {
        const auto path = Path(source);
        auto stem = Utf8(path.stem());
        if (Lower(stem) == "mod")
            stem = Utf8(path.parent_path().filename());
        static const pyre::Regex prefix(R"(^(?:nazi_zombie_|zm_))", pyre::IGNORECASE);
        static const pyre::Regex other(R"([^a-z0-9_]+)");
        auto name = prefix.Sub("", stem, 1);
        name = other.Sub("_", Lower(name));
        const auto first = name.find_first_not_of('_');
        name = first == std::string::npos ? std::string() : name.substr(first, name.find_last_not_of('_') - first + 1);
        if (name.empty())
            name = "map";
        return "zm_" + name.substr(0, 45) + "_waw";
    }

    std::string ValidateProject(const std::string& name)
    {
        static const pyre::Regex valid(R"(zm_[a-z0-9_]{1,58})");
        if (valid.FullMatch(name))
            return "";
        return "Map name must start with zm_ and use lowercase letters, numbers, and underscores (up to 61 characters).";
    }
} // namespace w2b::launcher

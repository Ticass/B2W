#include "pipeline/rumbles.h"

#include "core/encoding.h"
#include "core/files.h"
#include "gsc/port/util.h"
#include "gsc/tokens.h"
#include "weapons/stage.h"

#include <map>
#include <set>

namespace w2b::pipeline
{
    namespace
    {
        namespace fs = std::filesystem;
        using Names = std::set<std::string, std::less<>>;
        using Paths = std::map<std::string, fs::path, std::less<>>;

        void AddWeaponProfiles(const fs::path& content, Names& names)
        {
            const auto runtime = content / "weapons.runtime.json";
            if (!fs::exists(runtime))
                return;
            const auto info = weapons::ReadAssetJson(runtime);
            for (const auto& name : info.at("weapons"))
            {
                const auto source = content / "weapons" / files::FromUtf8(name.get_ref<const std::string&>());
                if (!fs::is_regular_file(source))
                    continue;
                const auto fields = weapons::ReadInfo(source);
                for (const auto& [key, value] : fields.items())
                {
                    const auto& profile = value.get_ref<const std::string&>();
                    if (!profile.empty() && gsc::Lower(key).ends_with("rumble"))
                        names.insert(profile);
                }
            }
        }

        void AddScriptProfiles(const fs::path& scripts, Names& names)
        {
            for (const auto& path : files::RGlob(scripts, ".gsc"))
            {
                const auto tokens = gsc::Tokenize(files::ReadText(path));
                for (std::size_t i = 0; i + 2 < tokens.size(); ++i)
                {
                    if (tokens[i + 1].text != "(" || tokens[i + 2].kind != gsc::Kind::String)
                        continue;
                    if (gsc::port::In(tokens[i].Low(), {"precacherumble", "playrumbleonentity", "playrumblelooponentity",
                                                        "playrumbleonposition"}))
                        names.insert(tokens[i + 2].text.substr(1, tokens[i + 2].text.size() - 2));
                }
            }
        }

        void AddGraphs(const fs::path& profile, const std::vector<fs::path>& roots, Paths& discovered, Names& missing)
        {
            std::string bytes;
            if (!files::ReadBytes(profile, bytes))
                throw std::runtime_error("cannot read " + files::Utf8(profile));
            const auto text = gsc::port::PyStrip(encoding::UniversalNewlines(encoding::Utf8Replace(bytes)));
            const auto fields = py::Split(text, "\\");
            if (fields.front() != "RUMBLE" || fields.size() % 2 != 1)
                throw std::invalid_argument("invalid rumble profile: " + files::Utf8(fs::path(profile).make_preferred()));
            auto search = roots;
            search.insert(search.begin(), profile.parent_path().parent_path());
            for (std::size_t i = 1; i < fields.size(); i += 2)
            {
                const auto name = fields[i + 1];
                if (name.empty() || !gsc::port::In(fields[i], {"lowRumbleFile", "highRumbleFile"}))
                    continue;
                weapons::OutputName("rumble graph", std::string(name));
                const auto relative = fs::path("rumble") / files::FromUtf8(name);
                const auto source = weapons::FindSource(search, relative);
                const auto key = weapons::PosixPath(relative);
                if (source.empty())
                    missing.insert(key);
                else
                    discovered[key] = source;
            }
        }
    }

    py::Json StageRumbles(const fs::path& project, const std::vector<fs::path>& roots, const fs::path& nativeRaw)
    {
        Names profiles, native, missing;
        AddWeaponProfiles(project / "content_source", profiles);
        AddScriptProfiles(project / "maps/mp/waw", profiles);
        Paths discovered;
        for (const auto& name : profiles)
        {
            weapons::OutputName("rumble", name);
            const auto relative = fs::path("rumble") / files::FromUtf8(name);
            const auto key = weapons::PosixPath(relative);
            auto source = weapons::FindSource(roots, relative);
            if (source.empty() && !nativeRaw.empty() && fs::is_regular_file(nativeRaw / relative))
            {
                source = nativeRaw / relative;
                native.insert(name);
            }
            if (source.empty())
            {
                missing.insert(key);
                continue;
            }
            discovered[key] = source;
            AddGraphs(source, roots, discovered, missing);
        }
        std::vector<std::string> staged;
        for (const auto& [name, source] : discovered)
        {
            const auto target = project / files::FromUtf8(name);
            std::error_code error;
            if (!fs::equivalent(source, target, error))
                weapons::CopySource(source, target);
            staged.push_back(name);
        }
        return {{"files", staged}, {"missing", missing}, {"native_profiles", native}};
    }

    py::Json StageRumblesRequest(const fs::path& path)
    {
        const auto request = weapons::ReadAssetJson(path);
        std::vector<fs::path> roots;
        for (const auto& root : request.at("roots"))
            roots.push_back(files::FromUtf8(root.get_ref<const std::string&>()));
        try
        {
            return StageRumbles(files::FromUtf8(request.at("project").get_ref<const std::string&>()), roots,
                                files::FromUtf8(request.value("nativeRaw", std::string{})));
        }
        catch (const std::invalid_argument& error)
        {
            return {{"invalid", error.what()}};
        }
        catch (const weapons::WeaponError& error)
        {
            return {{"invalid", error.what()}};
        }
    }
}

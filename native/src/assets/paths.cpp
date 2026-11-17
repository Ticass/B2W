#include "assets/resolver.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "weapons/weapon.h"

#include <algorithm>

namespace w2b::assets
{
    namespace
    {
        struct Location
        {
            std::string_view kind, directory, extension;
        };

        constexpr Location Locations[]{
            {"weapon", "weapons", ""}, {"flametable", "weapons", ""}, {"xmodel", "xmodel", ".json"},
            {"techniqueset", "waw_techniquesets", ".json"}, {"xanim", "xanim", ""},
            {"sound", "soundaliases", ".w2bsnd.json"}, {"loadedsound", "sound", ""},
            {"fx", "fx", ".w2bfx.json"}, {"physpreset", "physic", ""}, {"rawfile", "", ""},
        };
    }

    Path SourcePath(const Asset& asset)
    {
        const auto& kind = asset.first;
        auto name = asset.second;
        std::ranges::replace(name, '\\', '/');
        if (kind == "loadedsound")
        {
            name.erase(0, name.find_first_not_of('/'));
            const auto repeated = std::ranges::unique(name, [](auto a, auto b) { return a == '/' && b == '/'; });
            name.erase(repeated.begin(), repeated.end());
        }
        weapons::OutputName(kind, name);
        Path result;
        if (kind == "material")
            result = MaterialPath(name);
        else if (kind == "image")
            result = ImagePath(name, ".dds");
        else if (kind.starts_with("accuracy_"))
            result = Path("accuracy") / files::FromUtf8(std::string_view(kind).substr(9)) / files::FromUtf8(name);
        else if (const auto location = std::ranges::find(Locations, kind, &Location::kind); location != std::end(Locations))
            result = files::FromUtf8(location->directory) / files::FromUtf8(name + std::string(location->extension));
        else
            throw weapons::WeaponError("no compiled source path for asset kind " + kind);
        result = result.lexically_normal();
        return result.filename().empty() && result.has_parent_path() ? result.parent_path() : result;
    }

    Path FindFile(const Path& root, const Path& relative, bool loadedSound)
    {
        auto candidate = root == "." ? relative : root / relative;
        if (files::fs::is_regular_file(candidate))
            return candidate.make_preferred();
        if (loadedSound && files::fs::is_regular_file(candidate.replace_extension(".xwma")))
            return candidate.make_preferred();
        return {};
    }
}

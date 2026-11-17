#include "core/asset_paths.h"

#include "core/files.h"

#include <algorithm>

namespace w2b::assets
{
    std::filesystem::path MaterialPath(std::string name)
    {
        const bool generated = name.starts_with('*');
        if (generated)
        {
            std::ranges::replace(name, '*', '_');
            if (const auto suffix = name.find('('); suffix != name.npos)
                name.resize(suffix);
        }
        return std::filesystem::path(generated ? "materials/generated" : "materials") / files::FromUtf8(name + ".json");
    }

    std::filesystem::path ImagePath(std::string name, std::string_view extension)
    {
        std::ranges::replace(name, '*', '_');
        name += extension;
        return std::filesystem::path("images") / files::FromUtf8(name);
    }
}

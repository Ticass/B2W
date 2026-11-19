#include "world/zone_manifest.h"

#include "core/files.h"

#include <array>
#include <utility>

namespace w2b::world
{
    std::filesystem::path WriteMapZone(const std::filesystem::path& stage, const std::string& project, bool ipak,
                                       const MapZoneAssets& assets)
    {
        std::string text = ">game,T6\n>map,zm\n>level.@00012d3c,0\n";
        if (ipak)
            text += ">level.ipak_read," + project + "\n>ipak," + project + "\n";
        const std::array<std::pair<std::string_view, const std::vector<std::string>*>, 8> kinds{{
            {"image", &assets.image}, {"xmodel", &assets.xmodel}, {"zbarrier", &assets.zbarrier}, {"fx", &assets.fx},
            {"material", &assets.material}, {"script", &assets.script}, {"localize", &assets.localize}, {"rawfile", &assets.rawfile},
        }};
        for (const auto& [kind, entries] : kinds)
            for (const auto& entry : *entries)
            {
                text += kind;
                text += ',';
                text += entry;
                text += '\n';
            }
        const auto path = stage / "zone_source" / files::FromUtf8(project + ".zone");
        files::WriteText(path, text);
        return path;
    }
}

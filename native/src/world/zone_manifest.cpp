#include "world/zone_manifest.h"

#include "core/files.h"

#include <array>

namespace w2b::world
{
    std::filesystem::path WriteMapZone(const std::filesystem::path& stage, const std::string& project,
                                       bool ipak, const py::Json& assets)
    {
        std::string text = ">game,T6\n>map,zm\n>level.@00012d3c,0\n";
        if (ipak)
            text += ">level.ipak_read," + project + "\n>ipak," + project + "\n";
        constexpr std::array<std::string_view, 8> kinds{
            "image", "xmodel", "zbarrier", "fx", "material", "script", "localize", "rawfile"
        };
        for (const auto kind : kinds)
        {
            const auto entries = assets.find(kind);
            if (entries == assets.end())
                continue;
            for (const auto& entry : *entries)
            {
                text += kind;
                text += ',';
                text += entry.get_ref<const std::string&>();
                text += '\n';
            }
        }
        const auto path = stage / "zone_source" / files::FromUtf8(project + ".zone");
        files::WriteText(path, text);
        return path;
    }

    py::Json WriteMapZoneRequest(const std::filesystem::path& path)
    {
        const auto request = py::Json::parse(files::ReadText(path));
        const auto output = WriteMapZone(files::FromUtf8(request.at("stage").get<std::string>()),
                                        request.at("project").get_ref<const std::string&>(),
                                        request.at("ipak").get<bool>(), request.at("assets"));
        return files::Utf8(output);
    }
}

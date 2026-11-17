#include "world/grid_stage.h"

#include "core/files.h"
#include "world/grid_models.h"

namespace w2b::world
{
    py::Json StageGrid(const files::fs::path& request)
    {
        using py::Json;
        const auto args = Json::parse(files::ReadText(request));
        const auto path = [&](std::string_view key) { return files::FromUtf8(args.at(key).get<std::string>()); };
        const auto grid = ReadLightGrid(path("source"));
        if (!args.contains("models"))
            return WriteLightGrid(grid, path("destination"), {}, {});

        const auto collision = ReadCollisionWorld(path("clip"));
        const auto compensated = args.at("compensated").get<bool>();
        const WorldTracer tracer(collision, 0x2001, compensated);
        auto placements = Json::parse(files::ReadText(path("models")));
        const auto lights = Json::parse(files::ReadText(path("primary_lights")));
        auto samples = SampleModelLighting(grid, placements, tracer, lights.at("lights"), compensated);
        if ((grid.colors.size() + samples.palette.size()) / gridPaletteSize > 0xffff)
            throw std::runtime_error("static model lighting exceeds the T6 palette index range");
        auto placementsText = py::JsonDumps(placements, 1) + "\n";
        files::WriteText(path("models"), placementsText);
        return WriteLightGrid(grid, path("destination"), samples.palette, std::move(samples.summary));
    }
}

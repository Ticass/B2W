#include "world/grid_stage.h"

#include "core/files.h"
#include "world/grid_models.h"

namespace w2b::world
{
    py::Json StageGrid(const GridRequest& request)
    {
        using py::Json;
        const auto grid = ReadLightGrid(request.source);
        if (!request.models)
            return WriteLightGrid(grid, request.destination, {}, {});

        const auto collision = ReadCollisionWorld(request.models->clip);
        const WorldTracer tracer(collision, 0x2001, request.compensated);
        auto placements = Json::parse(files::ReadText(request.models->placements));
        const auto lights = Json::parse(files::ReadText(request.models->primaryLights));
        auto samples = SampleModelLighting(grid, placements, tracer, lights.at("lights"), request.compensated);
        if ((grid.colors.size() + samples.palette.size()) / gridPaletteSize > 0xffff)
            throw std::runtime_error("static model lighting exceeds the T6 palette index range");
        files::WriteText(request.models->placements, py::JsonDumps(placements, 1) + "\n");
        return WriteLightGrid(grid, request.destination, samples.palette, std::move(samples.summary));
    }
}

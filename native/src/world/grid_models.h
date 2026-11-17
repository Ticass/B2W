#pragma once

#include "world/light_grid.h"
#include "world/world_trace.h"

namespace w2b::world
{
    struct ModelGridSamples
    {
        std::vector<std::uint8_t> palette;
        py::Json summary;
    };

    ModelGridSamples SampleModelLighting(const LightGrid& grid, py::Json& placements, const WorldTracer& tracer,
                                        const py::Json& lights, bool compensated);
}

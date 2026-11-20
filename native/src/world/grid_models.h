#pragma once

#include "world/light_grid.h"
#include "world/world_trace.h"

namespace w2b::world
{
    struct ModelGridSamples
    {
        std::vector<std::uint8_t> palette;
        util::Json summary;
    };

    ModelGridSamples SampleModelLighting(const LightGrid& grid, util::Json& placements, const WorldTracer& tracer,
                                        const util::Json& lights, bool compensated);
}

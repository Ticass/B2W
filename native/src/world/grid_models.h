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

    // T4's lookup leaves out the corners it cannot trace to (GfxLightGridEntry.needsTrace); T6's blends every corner.
    // A point hidden from the space around it (inside a wall, most often black) takes what T4's lookup gives there.
    // Returns the palettes of the changed points, in the index order that starts at `firstColor`.
    std::vector<std::uint8_t> FillHiddenPoints(LightGrid& grid, const WorldTracer& tracer, std::size_t firstColor, util::Json& summary);

    ModelGridSamples SampleModelLighting(const LightGrid& grid, util::Json& placements, const WorldTracer& tracer,
                                        const util::Json& lights, bool compensated);
}

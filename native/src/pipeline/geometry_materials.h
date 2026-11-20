#pragma once

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <set>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::world
{
    struct RenderWorld;
}

namespace w2b::pipeline
{
    struct GeometryMaterials
    {
        std::set<std::string> sky, tools, shadowOnly, blendWeights, ownLightCasters;
        std::optional<std::string> skyImage;
    };

    GeometryMaterials PlanGeometryMaterials(world::RenderWorld& world, const std::vector<std::filesystem::path>& roots,
                                            extract::StockWaw* stock, util::Json& warnings);
}

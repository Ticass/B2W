#pragma once

#include "core/pytext.h"
#include "pipeline/bridge_geometry.h"

#include <filesystem>

namespace w2b::world { struct CollisionWorld; }

namespace w2b::pipeline
{
    // the map's entities, AI paths and zone graph; returns the navigation report
    py::Json StageBridgeNavigation(const NavigationRequest& request, const std::filesystem::path& gfx, const std::filesystem::path& project,
                                   const std::vector<std::filesystem::path>& roots, const world::CollisionWorld& clip);
}

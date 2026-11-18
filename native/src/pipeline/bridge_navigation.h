#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world { struct CollisionWorld; }

namespace w2b::pipeline
{
    py::Json StageBridgeNavigation(const py::Json& request, const std::filesystem::path& gfx, const std::filesystem::path& project,
                                   const std::vector<std::filesystem::path>& roots, const world::CollisionWorld& clip);
}

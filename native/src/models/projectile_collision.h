#pragma once
// Port of projectilecollision.py: missile-only collision proxies for hard
// render surfaces of static props that source movement brushes qualify.

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::models
{
    struct ProjectileCollisionRequest
    {
        std::filesystem::path gfx, clip, projectRoot;
        std::vector<std::filesystem::path> roots;
    };

    struct ProjectileCollision
    {
        std::vector<std::string> names;  // the proxy models the map zone carries
        std::optional<py::Json> entries; // the recovered placements, for the report; none when unsupported
        std::vector<std::string> warnings;
    };

    ProjectileCollision RecoverProjectileCollision(const ProjectileCollisionRequest& request);
} // namespace w2b::models

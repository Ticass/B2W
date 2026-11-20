#pragma once

#include "core/text.h"

#include <filesystem>
#include <optional>

namespace w2b::world
{
    struct CollisionWorld;

    util::Json StageCollisionMesh(const CollisionWorld& world, const std::filesystem::path& output, bool includeRecords);
    util::Json StageCollisionMesh(const std::filesystem::path& source, const std::filesystem::path& output,
                               bool includeRecords = false);
    std::size_t ExportCollisionFbx(const std::filesystem::path& source, const std::filesystem::path& target,
                                   std::optional<std::size_t> triangleLimit);
}

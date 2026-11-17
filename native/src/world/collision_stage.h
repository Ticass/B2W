#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <optional>

namespace w2b::world
{
    py::Json StageCollisionMesh(const std::filesystem::path& source, const std::filesystem::path& output,
                               bool includeRecords = false);
    std::size_t ExportCollisionFbx(const std::filesystem::path& source, const std::filesystem::path& target,
                                   std::optional<std::size_t> triangleLimit);
}

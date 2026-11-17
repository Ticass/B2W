#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world
{
    py::Json StageCollisionMesh(const std::filesystem::path& source, const std::filesystem::path& output,
                               bool includeRecords = false);
}

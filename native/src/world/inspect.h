#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world
{
    py::Json InspectWorld(const std::filesystem::path& gfx, const std::filesystem::path& clip);
}

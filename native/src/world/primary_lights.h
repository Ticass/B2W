#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world
{
    py::Json StagePrimaryLights(const std::filesystem::path& source, const std::filesystem::path& destination,
                                bool keepShadows);
}

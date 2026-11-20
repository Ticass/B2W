#pragma once

#include "core/text.h"

#include <filesystem>

namespace w2b::world
{
    util::Json StagePrimaryLights(const std::filesystem::path& source, const std::filesystem::path& destination,
                                bool keepShadows);
}

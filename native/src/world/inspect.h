#pragma once

#include "core/text.h"

#include <filesystem>

namespace w2b::world
{
    util::Json InspectWorld(const std::filesystem::path& gfx, const std::filesystem::path& clip);
    util::Json InspectStaticModels(const std::filesystem::path& gfx, const std::filesystem::path& models);
}

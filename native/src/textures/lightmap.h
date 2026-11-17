#pragma once

#include "textures/iwi.h"

#include <filesystem>
#include <optional>

namespace w2b::textures
{
    IwiImage BuildLightmap(const std::filesystem::path& secondary,
                          const std::optional<std::filesystem::path>& primary, unsigned index, bool diagnostic);
}

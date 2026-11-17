#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::effects
{
    py::Json StageEffects(const std::filesystem::path& request);
    py::Json StageOverlayEffects(const std::filesystem::path& request);
}

#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::weapons
{
    py::Json StageWaterBullets(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots,
                              bool hasWater, const py::Json& weapons);
    py::Json StageWaterBulletsRequest(const std::filesystem::path& request);
}

#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::world
{
    py::Json StageGrid(const std::filesystem::path& request);
}

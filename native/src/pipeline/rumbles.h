#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <vector>

namespace w2b::pipeline
{
    py::Json StageRumbles(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots,
                          const std::filesystem::path& nativeRaw);
    py::Json StageRumblesRequest(const std::filesystem::path& request);
}

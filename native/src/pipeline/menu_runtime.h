#pragma once

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    void ClearMenuRuntime(const std::filesystem::path& root, const py::Json& metadata);
    py::Json StageMenuRuntime(const std::filesystem::path& bo2, const std::filesystem::path& root,
                             const std::string& project, const std::filesystem::path& compat, bool aligned);
}

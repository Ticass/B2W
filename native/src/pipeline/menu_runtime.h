#pragma once

#include "core/text.h"

#include <filesystem>

namespace w2b::pipeline
{
    void ClearMenuRuntime(const std::filesystem::path& root, const util::Json& metadata);
    util::Json StageMenuRuntime(const std::filesystem::path& bo2, const std::filesystem::path& root,
                             const std::string& project, const std::filesystem::path& compat, bool aligned);
}

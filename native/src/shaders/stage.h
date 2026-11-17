#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::shaders
{
    // shaders.stage(roots, destination): translate every WaW shader program
    // under <root>/shader_bin and <root>/content_source/shader_bin (first
    // root wins per name) into destination, and write destination/stage.json.
    // roots are the Python str(Path) texts, so report paths read the same.
    py::Json Stage(const std::vector<std::string>& roots, const std::filesystem::path& destination);
} // namespace w2b::shaders

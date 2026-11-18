#pragma once
// The hooks on the ported map main (see hooks_stage.cpp).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, project, map_name, bo2_root, bo2_stock_perks, scripts,
    // characters, staged_animtrees}; returns {warnings, errors, content}
    py::Json HookMapScripts(const std::filesystem::path& request);
} // namespace w2b::pipeline

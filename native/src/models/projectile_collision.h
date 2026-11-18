#pragma once
// Port of projectilecollision.py: missile-only collision proxies for hard
// render surfaces of static props that source movement brushes qualify.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::models
{
    // request {gfx, clip, project_root, roots}; returns
    // {"names": [...], "entries": [...], "warnings": [...], "recorded": bool}
    py::Json RecoverProjectileCollision(const std::filesystem::path& request);
} // namespace w2b::models

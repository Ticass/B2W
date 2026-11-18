#pragma once
// t6bridge.stage_materials, per material (see materials_stage.cpp).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {names, roots, stock_materials, project_root, techset_root, rename,
    // image_prefix, donors, falloff_placement, light_shadows, layered_vertex_data,
    // native_fallbacks, scratch}; returns {"results": [...]} in name order
    py::Json StageMaterials(const std::filesystem::path& request);
} // namespace w2b::pipeline

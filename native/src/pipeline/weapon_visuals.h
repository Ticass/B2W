#pragma once
// weapons.stage_visuals: the weapons' materials through the measured
// material translator (world/model ones via stage_materials, effect ones on
// stock BO2 effect donors with their shader runtime), their original images
// (resolved through the IWD recovery that serves this operation) and the
// technique sets and shader programs they use; content_source/weapons.visuals.json.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    // request {roots, project, stock_materials, techset_dump, native_fallbacks,
    // bo2_raw, layered_vertex_data}; returns the visuals report plus "recovered"
    py::Json StageWeaponVisuals(const py::Json& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline

#pragma once
// wawhud.stage_assets: WaW's engine-owned HUD sprites (ammo counters, score
// bars) and the objective/score font glyphs as lossless RGBA 2D materials,
// with the glyph table and content_source/waw_hud.json.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, roots, stock_waw}; returns {result, stock_prepared}
    py::Json StageWawHud(const std::filesystem::path& request);
} // namespace w2b::pipeline

#pragma once
// wawhud.stage_assets: WaW's engine-owned HUD sprites (ammo counters, score
// bars) and the objective/score font glyphs as lossless RGBA 2D materials,
// with the glyph table and content_source/waw_hud.json.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    // request {project_root, roots, stock_waw}; returns {result, stock_prepared}
    py::Json StageWawHud(const std::filesystem::path& request);
    // wawmenu.stage_assets, served by the IWD recovery (its images resolve
    // through it): request {project_root, roots, stock_waw, localized};
    // returns {menus, materials, glyphs, missing, recovered, stock_prepared}
    py::Json StageWawMenuArt(const py::Json& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline

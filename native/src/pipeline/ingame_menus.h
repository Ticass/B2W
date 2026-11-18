#pragma once
// wawmenu.stage_source: WaW's in-game (pause) menus translated for T6 LUI.
// The Mod Tools raw/ui menulists are preprocessed as WaW's menu compiler does
// (PC defined), every menuDef parsed, and the closure reachable from
// pausedmenu encoded as zm/waw_menus.csv with content_source/waw_menu.json.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, raw_roots, mapname}; returns {result} or, for a
    // wawmenu.MenuSourceError, {menu_error}
    py::Json StageWawMenus(const std::filesystem::path& request);
} // namespace w2b::pipeline

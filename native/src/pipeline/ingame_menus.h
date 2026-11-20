#pragma once
// wawmenu.stage_source: WaW's in-game (pause) menus translated for T6 LUI.
// The Mod Tools raw/ui menulists are preprocessed as WaW's menu compiler does
// (PC defined), every menuDef parsed, and the closure reachable from
// pausedmenu encoded as zm/waw_menus.csv with content_source/waw_menu.json.

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct MenuStage
    {
        util::Json result;                  // content_source/waw_menu.json, as written
        std::optional<std::string> error; // the WaW menu sources are missing or incomplete
    };

    MenuStage StageWawMenus(const std::filesystem::path& project, const std::vector<std::filesystem::path>& rawRoots, const std::string& mapname);
} // namespace w2b::pipeline

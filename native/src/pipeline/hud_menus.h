#pragma once
// The HUD menus a map overrides (weaponinfo, dpad, competitivemodescores): the
// compiled menuDefs of its zones that differ from every stock WaW one, as rows
// of zm/waw_hud_menus.csv that waw_hud.lua draws in place of the stock HUD's.

#include "core/text.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    // a compiled WaW expression (operators as numbers, operands as values) as
    // the RPN waw_menu.lua evaluates; throws std::runtime_error on one it cannot read
    std::string HudExpression(const util::Json& tokens);

    struct HudMenus
    {
        std::vector<std::vector<std::string>> rows; // key, then the columns waw_hud.lua reads
        std::set<std::string> fonts;                // font roles the texts use (see FontRole)
        std::vector<std::string> menus;             // the overridden ones
        util::Json warnings = util::Json::array();
    };

    // the font role T4's UI_GetFontHandle picks at 1080 lines
    std::string HudFontRole(int font, double scale);

    // `sprite` stages a material's image and names the material that draws it (nullopt: no such image)
    HudMenus ReadHudMenus(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots, extract::StockWaw* stock,
                          const std::function<std::optional<std::string>(const std::string&)>& sprite);
} // namespace w2b::pipeline

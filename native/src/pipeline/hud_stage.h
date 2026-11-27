#pragma once
// wawhud.stage_assets: WaW's engine-owned HUD sprites (ammo counters, score
// bars) and the objective/score font glyphs as lossless RGBA 2D materials,
// with the glyph table and content_source/waw_hud.json.

#include "core/text.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    // the original WaW HUD sprites and glyphs; returns content_source/waw_hud.json as written.
    // recovery: images resolve through the map's IWDs as WaW loads them (a font atlas a zone dump took from another IWD)
    util::Json StageWawHud(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots, extract::StockWaw* stock,
                           textures::IwdRecovery* recovery = nullptr);

    struct MenuArt
    {
        std::size_t menus = 0, materials = 0, glyphs = 0;
        std::vector<std::string> missing; // drawn empty
        util::Json recovered;               // IWD images decoded for the menus (wavelet.recovered)
    };

    // wawmenu.stage_assets, served by the IWD recovery (its images resolve through it);
    // localized: the map's localized strings, key -> text
    MenuArt StageWawMenuArt(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots, extract::StockWaw* stock,
                            const std::map<std::string, std::string>& localized, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline

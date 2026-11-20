#pragma once
// gscport's module constants and compat/gsc_api.json.

#include "core/text.h"

#include <filesystem>
#include <string>

namespace w2b::gsc::port
{
    inline constexpr const char* COMPAT = "maps\\mp\\waw\\_waw2bo2_compat";
    inline constexpr const char* FIXES = "maps\\mp\\waw\\_waw2bo2_fixes";
    inline constexpr const char* STUBS = "maps\\mp\\waw\\_waw2bo2_stubs";
    inline constexpr const char* ASSETS = "maps\\mp\\waw\\_waw2bo2_assets";
    inline constexpr const char* CORE = "maps\\mp\\waw\\_waw2bo2_core";
    inline constexpr const char* PRECACHE = "maps\\mp\\waw\\_waw2bo2_precache";
    inline constexpr const char* ONEWAY = "maps\\mp\\waw\\_waw2bo2_oneway";
    inline constexpr const char* WATER_BULLETS = "maps\\mp\\waw\\_waw2bo2_water_bullets";
    inline constexpr const char* ZOMBIEMODE = "maps\\_zombiemode";
    inline constexpr const char* PERKS = "maps\\_zombiemode_perks";
    inline constexpr const char* GAMESKILL = "maps\\_gameskill";
    inline constexpr const char* LOAD = "maps\\_load";
    inline constexpr const char* WEAPONS_FRAMEWORK = "maps\\_zombiemode_weapons";
    inline constexpr const char* SCORE_FRAMEWORK = "maps\\_zombiemode_score";
    inline constexpr const char* OWNED_ZM = "maps\\mp\\waw\\_waw2bo2_zm";
    inline constexpr const char* HUD_MATERIAL_TABLE = "maps\\mp\\waw\\_waw2bo2_hud_materials";
    inline constexpr const char* FALLBACK = "GSC fallback (reported; BO2's behaviour runs)";

    // the compat folder (src/waw2bo2/compat); set once before any use
    void SetCompatDir(const std::filesystem::path& folder);
    const std::filesystem::path& CompatDir();

    // gscport.API: compat/gsc_api.json
    const util::Json& Api();

    // gscport.norm
    std::string Norm(std::string path);
} // namespace w2b::gsc::port

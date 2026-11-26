#pragma once
// The menus a map's scripts open (openMenu( game["tom_music_player"] )): the
// compiled menuDefs of the map's own zones, in the parsed form of
// ingame_menus.cpp, so that zm/waw_menus.csv carries them beside WaW's pause
// menu and waw_menu.lua runs them when T6's openmenu asks LUI for them.

#include "core/text.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    // the names the ported scripts pass to openMenu / openMenuNoMouse, through game["..."] too
    std::set<std::string> ScriptOpenedMenus(const std::filesystem::path& project);

    // a compiled T4 menuDef (oat::ReadMenus) as a parsed menu source; "compiled" holds the
    // visible/exp columns already as RPN, "keys" the execKey scripts
    util::Json CompiledMenuSource(const std::string& name, const util::Json& menu);

    struct ScriptMenus
    {
        std::map<std::string, util::Json> menus; // lower-case name -> parsed source
        std::vector<std::string> warnings;
    };

    // the opened menus the map's zones define (the mod's zone over the map's)
    ScriptMenus ReadScriptMenus(const std::filesystem::path& project, const std::vector<std::filesystem::path>& mapRoots);
} // namespace w2b::pipeline

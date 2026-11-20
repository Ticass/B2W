#pragma once
// The weapons mod.ff carries: animnotes.stage_weapon_events (weapon sound
// notes added to the raw v17 xanims) and weapons.stage_runtime (which staged
// weapons convert whole, BO2 equivalents for absent models (bo2equiv), the
// written T6 weapon files, zone lines and content_source/weapons.runtime.json).

#include "core/text.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct WeaponRuntimeRequest
    {
        std::filesystem::path content; // the project's content_source
        std::string ipak;
        std::optional<std::filesystem::path> bo2Root;
        std::map<std::string, std::set<std::string>> loaded; // stock T6 assets by kind
        std::set<std::string> convertedFx, reserved;         // reserved: names BO2's zones already define
    };

    // weapons.stage_runtime: content_source/weapons.runtime.json, as written
    util::Json StageWeaponRuntime(const WeaponRuntimeRequest& request);
    // the rest of t6bridge._stage_weapon_runtime: the map's string tables
    // (stage_map_tables), maps/mp/waw/_waw2bo2_assets.gsc and the wall buys
    // (entities.map_wall_buys). request {project_root, fx_table, waw_map,
    // weapons, map_table_roots, table_roots}
    struct WeaponFinishRequest
    {
        std::filesystem::path projectRoot;
        std::map<std::string, std::string> fxTable;             // script-loaded WaW effect -> converted
        std::optional<std::string> wawMap;
        std::map<std::string, std::string> weapons;             // weapons.runtime.json table: WaW -> BO2 name
        std::vector<std::filesystem::path> mapTableRoots, tableRoots;
    };

    struct WeaponRuntimeFinish
    {
        std::vector<std::string> mapTables;    // the map's own string tables, staged as waw/<path>
        std::vector<std::string> wallBuyNotes; // UNSUPPORTED_WALLBUY lines
    };

    WeaponRuntimeFinish FinishWeaponRuntime(const WeaponFinishRequest& request);
} // namespace w2b::pipeline

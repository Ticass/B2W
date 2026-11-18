#pragma once
// The weapons mod.ff carries: animnotes.stage_weapon_events (weapon sound
// notes added to the raw v17 xanims) and weapons.stage_runtime (which staged
// weapons convert whole, BO2 equivalents for absent models (bo2equiv), the
// written T6 weapon files, zone lines and content_source/weapons.runtime.json).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {content, ipak, bo2_root, loaded {kind: [names]}, converted_fx,
    // reserved}; returns weapons.stage_runtime's result
    py::Json StageWeaponRuntime(const std::filesystem::path& request);
    // the rest of t6bridge._stage_weapon_runtime: the map's string tables
    // (stage_map_tables), maps/mp/waw/_waw2bo2_assets.gsc and the wall buys
    // (entities.map_wall_buys). request {project_root, fx_table, waw_map,
    // weapons, map_table_roots, table_roots}; returns {map_tables, wall_buy_notes}
    py::Json FinishWeaponRuntime(const std::filesystem::path& request);
} // namespace w2b::pipeline

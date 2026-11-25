#pragma once
// WaW zone assets that say what the game plays on an event: the impact effect
// table and the destructibles, read from a zone loaded in this process.

#include "oat/waw_assets.h"

namespace w2b::oat
{
    // JSON text: the zone's impact table, 16 rows of 35 effect names ("" for
    // none): 31 surfaces, then flesh body nonfatal/fatal and head
    // nonfatal/fatal; "null" when the zone has no table
    std::string ReadImpactTable(const WawZone& zone);
    // JSON text: destructible name -> its base model and pieces
    std::string ReadDestructibles(const WawZone& zone);
} // namespace w2b::oat

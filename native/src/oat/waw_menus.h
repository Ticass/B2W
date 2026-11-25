#pragma once
// WaW's compiled menus (menuDef_t) read from a zone loaded in this process.

#include "oat/waw_assets.h"

namespace w2b::oat
{
    // JSON text: menu name -> its window, expressions and items, for every menu the zone defines
    std::string ReadMenus(const WawZone& zone);
} // namespace w2b::oat

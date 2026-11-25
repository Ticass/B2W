#pragma once

#include "Asset/IAssetCreator.h"
#include "Game/T6/T6.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"

#include <memory>

namespace fx
{
    // waw2bo2: builds the map's FxImpactTable from fx/<name>.w2bimpacts.json: 21 rows of 36 effect names
    // ("" for none). The game looks the table named "" up, whatever the zone definition calls it.
    std::unique_ptr<AssetCreator<T6::AssetImpactFx>> CreateJsonLoaderImpactT6(MemoryManager& memory, ISearchPath& searchPath);
} // namespace fx

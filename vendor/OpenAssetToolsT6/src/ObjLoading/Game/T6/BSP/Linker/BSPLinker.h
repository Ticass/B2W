#pragma once

#include "Asset/IAssetCreator.h"
#include "Game/T6/BSP/BSP.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"
#include "Zone/Zone.h"

namespace BSP
{
    class BSPLinker
    {
    public:
        BSPLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context, Zone& zone);

        [[nodiscard]] bool LinkBSP(const BSPData& bsp) const;

    private:
        void AddEmptyFootstepTableAsset(const std::string& assetName) const;
        [[nodiscard]] bool AddDefaultRequiredAssets(const BSPData& bsp) const;

        MemoryManager& m_memory;
        ISearchPath& m_search_path;
        AssetCreationContext& m_context;
        Zone& m_zone;
    };
} // namespace BSP

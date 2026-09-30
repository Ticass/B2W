#pragma once

#include "Asset/IAssetCreator.h"
#include "Game/T6/BSP/BSP.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"
#include "Zone/Zone.h"

#include <vector>

namespace BSP
{
    class GameWorldMpLinker
    {
    public:
        GameWorldMpLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context, Zone& zone);

        // Links the gameworld and adds it (with the script strings its path nodes use) to the zone.
        [[nodiscard]] bool LinkGameWorldMp(const BSPData& bsp);

    private:
        bool LoadPaths(T6::PathData& path);
        scr_string_t ScriptString(const std::string& value);
        void BuildNodeTree(T6::PathData& path) const;

        MemoryManager& m_memory;
        ISearchPath& m_search_path;
        AssetCreationContext& m_context;
        Zone& m_zone;
        std::vector<scr_string_t> m_script_strings;
    };
} // namespace BSP

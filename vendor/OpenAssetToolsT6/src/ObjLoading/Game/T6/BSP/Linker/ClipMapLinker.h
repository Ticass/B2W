#pragma once

#include <map>
#include <string>
#include <vector>

#include "Asset/IAssetCreator.h"
#include "Game/T6/BSP/BSP.h"
#include "Game/T6/BSP/BSPCalculation.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"
#include "Zone/Zone.h"

namespace BSP
{
    class ClipMapLinker
    {
    public:
        ClipMapLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context, Zone& zone);
        const std::vector<scr_string_t>& ScriptStrings() const { return m_script_strings; }

        [[nodiscard]] T6::clipMap_t* LinkClipMap(const BSPData& bsp);

        // Native brush collision read from BSP/brushes.json and BSP/submodels.json
        struct BrushSource
        {
            T6::vec3_t mins;
            T6::vec3_t maxs;
            int contents;
            int axialCflags[2][3];
            int axialSflags[2][3];
            std::vector<T6::cplane_s> sidePlanes;
            std::vector<std::pair<int, int>> sideFlags; // cflags, sflags
            std::vector<T6::vec3_t> verts;
        };

        struct SubModelSource
        {
            T6::vec3_t mins;
            T6::vec3_t maxs;
            size_t firstBrush;
            size_t brushCount;
            size_t firstTerrain = 0;
            size_t terrainCount = 0;
            int terrainContents = 0;
        };

    private:
        void LoadBoxData(T6::clipMap_t& clipMap) const;
        void LoadVisibility(T6::clipMap_t& clipMap) const;
        bool LoadDynEnts(T6::clipMap_t& clipMap);
        [[nodiscard]] size_t MapDynEntCount() const;
        bool LoadRopesAndConstraints(T6::clipMap_t& clipMap);
        void LoadSubModelCollision(T6::clipMap_t& clipMap, const BSPData& bsp) const;
        bool LoadXModelCollision(T6::clipMap_t& clipMap) const;

        void AddAABBTreeFromLeaf(T6::clipMap_t& clipMap, const BSPTree& tree, size_t& outParentCount, size_t& outParentStartIndex);
        int16_t LoadBSPNode(T6::clipMap_t& clipMap, const BSPTree& tree);
        bool LoadBSPTree(T6::clipMap_t& clipMap, const BSPData& bsp);
        bool LoadPartitions(T6::clipMap_t& clipMap, const BSPData& bsp);
        bool LoadClipMaterials(T6::clipMap_t& clipMap);
        [[nodiscard]] int LeafTerrainContents(const BSPTree& tree) const;
        bool LoadWorldCollision(T6::clipMap_t& clipMap, const BSPData& bsp);
        bool LoadWalkableEdges(T6::clipMap_t& clipMap);

        bool ReadBrushFile();
        uint16_t BuildLeafBrushNode(std::vector<uint16_t>& brushIds, std::vector<T6::cLeafBrushNode_s>& nodes,
                                    std::vector<uint16_t>& leafBrushes, std::vector<size_t>& leafOffsets) const;
        void LinkBrushes(T6::clipMap_t& clipMap, size_t firstBrushPlane);

        MemoryManager& m_memory;
        ISearchPath& m_search_path;
        AssetCreationContext& m_context;
        Zone& m_zone;
        std::vector<scr_string_t> m_script_strings;

        std::vector<BrushSource> brushSources;
        size_t worldBrushCount = 0;
        std::vector<SubModelSource> subModelSources;
        std::vector<T6::cplane_s> planeVec;
        std::vector<T6::cNode_t> nodeVec;
        std::vector<T6::cLeaf_s> leafVec;
        std::vector<size_t> leafFirstAabb; // full index of each leaf's first parent aabb
        size_t pendingLeafAabbStart = 0;
        std::vector<T6::CollisionAabbTree> AABBTreeVec;
        size_t highestLeafObjectCount = 0;
        std::map<std::string, uint16_t> materialByFbx; // collision FBX material -> clip material
        std::vector<int> materialContents;              // clip material contents
        std::vector<unsigned int> materialModels;       // authored cmodel owner
        std::vector<uint16_t> partitionMaterials;       // clip material of each partition
    };
} // namespace BSP

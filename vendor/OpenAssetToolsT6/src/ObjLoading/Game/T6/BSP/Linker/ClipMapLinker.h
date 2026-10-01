#pragma once

#include <map>
#include <string>
#include <vector>

#include "Asset/IAssetCreator.h"
#include "Game/T6/BSP/BSP.h"
#include "Game/T6/BSP/BSPCalculation.h"
#include "SearchPath/ISearchPath.h"
#include "Utils/MemoryManager.h"

namespace BSP
{
    class ClipMapLinker
    {
    public:
        ClipMapLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context);

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
        };

    private:
        void LoadBoxData(T6::clipMap_t& clipMap) const;
        void LoadVisibility(T6::clipMap_t& clipMap) const;
        void LoadDynEnts(T6::clipMap_t& clipMap) const;
        void LoadRopesAndConstraints(T6::clipMap_t& clipMap) const;
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
        std::vector<uint16_t> partitionMaterials;       // clip material of each partition
    };
} // namespace BSP

#include "ClipMapLinker.h"

#include "Game/T6/BSP/BSPUtil.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <tuple>

using namespace nlohmann;
using namespace T6;

namespace BSP
{
    ClipMapLinker::ClipMapLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context)
        : m_memory(memory),
          m_search_path(searchPath),
          m_context(context)
    {
    }

    void ClipMapLinker::LoadDynEnts(clipMap_t& clipMap) const
    {
        uint16_t dynEntCount = 0;
        clipMap.originalDynEntCount = dynEntCount;
        clipMap.dynEntCount[0] = clipMap.originalDynEntCount + 256; // the game allocs 256 empty dynents, as they may be used ingame
        clipMap.dynEntCount[1] = 0;
        clipMap.dynEntCount[2] = 0;
        clipMap.dynEntCount[3] = 0;

        clipMap.dynEntClientList[0] = m_memory.Alloc<DynEntityClient>(clipMap.dynEntCount[0]);
        clipMap.dynEntClientList[1] = nullptr;

        clipMap.dynEntServerList[0] = nullptr;
        clipMap.dynEntServerList[1] = nullptr;

        clipMap.dynEntCollList[0] = m_memory.Alloc<DynEntityColl>(clipMap.dynEntCount[0]);
        clipMap.dynEntCollList[1] = nullptr;
        clipMap.dynEntCollList[2] = nullptr;
        clipMap.dynEntCollList[3] = nullptr;

        clipMap.dynEntPoseList[0] = m_memory.Alloc<DynEntityPose>(clipMap.dynEntCount[0]);
        clipMap.dynEntPoseList[1] = nullptr;

        clipMap.dynEntDefList[0] = m_memory.Alloc<DynEntityDef>(clipMap.dynEntCount[0]);
        clipMap.dynEntDefList[1] = nullptr;
    }

    void ClipMapLinker::LoadVisibility(clipMap_t& clipMap) const
    {
        // Only use one visbility cluster for the entire map
        clipMap.numClusters = 1;
        clipMap.vised = 0;
        clipMap.clusterBytes = ((clipMap.numClusters + 63) >> 3) & 0xFFFFFFF8;
        clipMap.visibility = m_memory.Alloc<char>(clipMap.clusterBytes);
        // Official maps set visibility to all 0xFF
        memset(clipMap.visibility, 0xFF, clipMap.clusterBytes);
    }

    void ClipMapLinker::LoadBoxData(clipMap_t& clipMap) const
    {
        // box_model and box_brush are what are used by game traces as "temporary" collision when
        //  no brush or model is specified to do the trace with.
        // All values in this function are taken from official map BSPs

        // for some reason the maxs are negative, and mins are positive
        // float box_mins = 3.4028235e38;
        // float box_maxs = -3.4028235e38;
        // hack: the floats above can't be safely converted to 32 bit floats, and the game requires them to be exact
        //  so we use the hex representation and set it using int pointers.
        unsigned int box_mins = 0x7F7FFFFF;
        unsigned int box_maxs = 0xFF7FFFFF;
        *(reinterpret_cast<unsigned int*>(&clipMap.box_model.leaf.mins.x)) = box_mins;
        *(reinterpret_cast<unsigned int*>(&clipMap.box_model.leaf.mins.y)) = box_mins;
        *(reinterpret_cast<unsigned int*>(&clipMap.box_model.leaf.mins.z)) = box_mins;
        *(reinterpret_cast<unsigned int*>(&clipMap.box_model.leaf.maxs.x)) = box_maxs;
        *(reinterpret_cast<unsigned int*>(&clipMap.box_model.leaf.maxs.y)) = box_maxs;
        *(reinterpret_cast<unsigned int*>(&clipMap.box_model.leaf.maxs.z)) = box_maxs;

        clipMap.box_model.leaf.brushContents = -1;
        clipMap.box_model.leaf.terrainContents = 0;
        clipMap.box_model.leaf.cluster = 0;
        clipMap.box_model.leaf.collAabbCount = 0;
        clipMap.box_model.leaf.firstCollAabbIndex = 0;
        clipMap.box_model.leaf.leafBrushNode = 0;
        clipMap.box_model.mins.x = 0.0f;
        clipMap.box_model.mins.y = 0.0f;
        clipMap.box_model.mins.z = 0.0f;
        clipMap.box_model.maxs.x = 0.0f;
        clipMap.box_model.maxs.y = 0.0f;
        clipMap.box_model.maxs.z = 0.0f;
        clipMap.box_model.radius = 0.0f;
        clipMap.box_model.info = nullptr;

        clipMap.box_brush = m_memory.Alloc<cbrush_t>();
        clipMap.box_brush->axial_sflags[0][0] = -1;
        clipMap.box_brush->axial_sflags[0][1] = -1;
        clipMap.box_brush->axial_sflags[0][2] = -1;
        clipMap.box_brush->axial_sflags[1][0] = -1;
        clipMap.box_brush->axial_sflags[1][1] = -1;
        clipMap.box_brush->axial_sflags[1][2] = -1;
        clipMap.box_brush->axial_cflags[0][0] = -1;
        clipMap.box_brush->axial_cflags[0][1] = -1;
        clipMap.box_brush->axial_cflags[0][2] = -1;
        clipMap.box_brush->axial_cflags[1][0] = -1;
        clipMap.box_brush->axial_cflags[1][1] = -1;
        clipMap.box_brush->axial_cflags[1][2] = -1;
        clipMap.box_brush->contents = -1;
        clipMap.box_brush->mins.x = 0.0f;
        clipMap.box_brush->mins.y = 0.0f;
        clipMap.box_brush->mins.z = 0.0f;
        clipMap.box_brush->maxs.x = 0.0f;
        clipMap.box_brush->maxs.y = 0.0f;
        clipMap.box_brush->maxs.z = 0.0f;
        clipMap.box_brush->numsides = 0;
        clipMap.box_brush->numverts = 0;
        clipMap.box_brush->sides = nullptr;
        clipMap.box_brush->verts = nullptr;
    }

    void ClipMapLinker::LoadRopesAndConstraints(clipMap_t& clipMap) const
    {
        clipMap.num_constraints = 0; // max 511
        clipMap.constraints = nullptr;

        // The game allocates 32 empty ropes
        clipMap.max_ropes = 32; // max 300
        clipMap.ropes = m_memory.Alloc<rope_t>(clipMap.max_ropes);
    }

    void ClipMapLinker::LoadSubModelCollision(clipMap_t& clipMap, const BSPData& bsp) const
    {
        // Submodels are used for the world and map ent collision (triggers, bomb zones, etc)
        auto gfxWorldAsset = m_context.LoadDependency<AssetGfxWorld>(bsp.bspName);
        assert(gfxWorldAsset != nullptr);
        auto* gfxWorld = gfxWorldAsset->Asset();

        // Only the world submodel here; brush models are added by LinkBrushes
        assert(gfxWorld->modelCount >= 1);

        clipMap.numSubModels = 1;
        clipMap.cmodels = m_memory.Alloc<cmodel_t>(clipMap.numSubModels);

        auto* gfxModel = &gfxWorld->models[0];
        clipMap.cmodels[0].mins.x = gfxModel->bounds[0].x;
        clipMap.cmodels[0].mins.y = gfxModel->bounds[0].y;
        clipMap.cmodels[0].mins.z = gfxModel->bounds[0].z;
        clipMap.cmodels[0].maxs.x = gfxModel->bounds[1].x;
        clipMap.cmodels[0].maxs.y = gfxModel->bounds[1].y;
        clipMap.cmodels[0].maxs.z = gfxModel->bounds[1].z;
        clipMap.cmodels[0].radius = DistBetweenPoints(clipMap.cmodels[0].mins, clipMap.cmodels[0].maxs) / 2;

        // The world model is also reached directly by model-index-0 traces.
        // Keep its leaf collision-enabled; LinkBrushes fills in the world
        // brush root once the brush tree has been constructed.
        clipMap.cmodels[0].leaf.firstCollAabbIndex = 0;
        clipMap.cmodels[0].leaf.collAabbCount = 0;
        clipMap.cmodels[0].leaf.brushContents = 0;
        clipMap.cmodels[0].leaf.terrainContents = BSPEditableConstants::LEAF_TERRAIN_CONTENTS;
        clipMap.cmodels[0].leaf.mins.x = 0.0f;
        clipMap.cmodels[0].leaf.mins.y = 0.0f;
        clipMap.cmodels[0].leaf.mins.z = 0.0f;
        clipMap.cmodels[0].leaf.maxs.x = 0.0f;
        clipMap.cmodels[0].leaf.maxs.y = 0.0f;
        clipMap.cmodels[0].leaf.maxs.z = 0.0f;
        clipMap.cmodels[0].leaf.leafBrushNode = 0;
        clipMap.cmodels[0].leaf.cluster = 0;

        clipMap.cmodels[0].info = nullptr; // always set to 0
    }

    bool ClipMapLinker::LoadXModelCollision(clipMap_t& clipMap) const
    {
        // BSP/staticmodels.json (game coordinates), the source clipmap's static models:
        // { "staticModels": [ { "name", "contents", "origin": [3], "invScaledAxis": [9],
        //                       "absmin": [3], "absmax": [3] } ] }
        // The game links these into its world sectors at load and line-traces
        // each one against its xmodel's collSurfs; they must not become brushes
        // (pmove/missile brush gathering keeps only 512 brushes per query).
        clipMap.numStaticModels = 0;
        clipMap.staticModelList = nullptr;

        const auto filePath = GetFileNameForBSPAsset("staticmodels.json");
        const auto file = m_search_path.Open(filePath);
        if (!file.IsOpen())
            return true;

        json js;
        try
        {
            js = json::parse(*file.m_stream);
        }
        catch (const json::exception& e)
        {
            con::error("JSON error when parsing {}: {}", filePath, e.what());
            return false;
        }

        const auto vec = [](const json& v, const size_t offset = 0)
        {
            return vec3_t{{.x = v.at(offset).get<float>(), .y = v.at(offset + 1).get<float>(), .z = v.at(offset + 2).get<float>()}};
        };

        const auto& entries = js.at("staticModels");
        if (entries.size() > std::numeric_limits<uint16_t>::max())
        {
            con::error("ERROR: {} collision static models exceed the uint16 sector link limit", entries.size());
            return false;
        }
        std::vector<cStaticModel_s> models;
        models.reserve(entries.size());
        size_t withoutCollSurfs = 0;
        for (const auto& entry : entries)
        {
            const auto name = entry.at("name").get<std::string>();
            auto* xModelAsset = m_context.LoadDependency<AssetXModel>(name);
            if (!xModelAsset)
            {
                con::error("ERROR! collision static model xmodel {} could not be loaded!", name);
                return false;
            }
            cStaticModel_s model{};
            model.xmodel = xModelAsset->Asset();
            if (!model.xmodel->collSurfs || model.xmodel->numCollSurfs <= 0)
                withoutCollSurfs++;
            model.contents = entry.at("contents").get<int>();
            model.origin = vec(entry.at("origin"));
            const auto& axis = entry.at("invScaledAxis");
            for (auto row = 0u; row < 3u; row++)
                model.invScaledAxis[row] = vec(axis, row * 3);
            model.absmin = vec(entry.at("absmin"));
            model.absmax = vec(entry.at("absmax"));
            models.emplace_back(model);
        }
        if (withoutCollSurfs)
            con::warn("{} collision static models have an xmodel without collSurfs; they cannot be hit", withoutCollSurfs);

        clipMap.numStaticModels = static_cast<unsigned int>(models.size());
        if (!models.empty())
        {
            clipMap.staticModelList = m_memory.Alloc<cStaticModel_s>(models.size());
            memcpy(clipMap.staticModelList, models.data(), sizeof(cStaticModel_s) * models.size());
        }
        con::info("Linked {} collision static models", models.size());
        return true;
    }

    void ClipMapLinker::AddAABBTreeFromLeaf(clipMap_t& clipMap, const BSPTree& tree, size_t& outParentCount, size_t& outParentStartIndex)
    {
        assert(tree.isLeaf);

        size_t leafObjectCount = tree.leaf->GetObjectCount();
        assert(leafObjectCount > 0);
        highestLeafObjectCount = std::max(leafObjectCount, highestLeafObjectCount);

        // Parents are material-homogeneous: the engine checks the PARENT's
        // clip material contents against the trace mask before visiting its
        // children (sub_6B9730), then each child's own material.
        std::vector<int> partitionIndices;
        partitionIndices.reserve(leafObjectCount);
        for (size_t objectIdx = 0; objectIdx < leafObjectCount; objectIdx++)
            partitionIndices.emplace_back(tree.leaf->GetObject(objectIdx)->partitionIndex);
        const auto materialOf = [this](const int partitionIndex)
        {
            return partitionIndex < static_cast<int>(partitionMaterials.size()) ? partitionMaterials[partitionIndex] : uint16_t{0};
        };
        std::ranges::stable_sort(partitionIndices, {}, materialOf);

        const auto partitionBounds = [&clipMap](const int partitionIndex, vec3_t& mins, vec3_t& maxs, const bool first)
        {
            const CollisionPartition& partition = clipMap.partitions[partitionIndex];
            for (int uindIdx = 0; uindIdx < partition.nuinds; uindIdx++)
            {
                const vec3_t vert = clipMap.verts[clipMap.info.uinds[partition.fuind + uindIdx]];
                if (first && uindIdx == 0)
                {
                    mins = vert;
                    maxs = vert;
                }
                UpdateAABBWithPoint(vert, mins, maxs);
            }
        };

        // chunks: runs of one material, at most MAX_AABB_TREE_CHILDREN children each
        std::vector<std::pair<size_t, size_t>> chunks;
        for (size_t start = 0; start < partitionIndices.size();)
        {
            auto end = start + 1;
            while (end < partitionIndices.size() && end - start < BSPGameConstants::MAX_AABB_TREE_CHILDREN
                   && materialOf(partitionIndices[end]) == materialOf(partitionIndices[start]))
                end++;
            chunks.emplace_back(start, end);
            start = end;
        }

        const size_t parentAABBArrayIndex = AABBTreeVec.size();
        AABBTreeVec.resize(AABBTreeVec.size() + chunks.size());
        for (size_t parentIdx = 0; parentIdx < chunks.size(); parentIdx++)
        {
            const auto [start, end] = chunks[parentIdx];
            const auto material = materialOf(partitionIndices[start]);
            vec3_t parentMins{}, parentMaxs{};
            for (auto i = start; i < end; i++)
                partitionBounds(partitionIndices[i], parentMins, parentMaxs, i == start);

            CollisionAabbTree parentAABB{};
            parentAABB.origin = CalcMiddleOfAABB(parentMins, parentMaxs);
            parentAABB.halfSize = CalcHalfSizeOfAABB(parentMins, parentMaxs);
            parentAABB.materialIndex = material;
            parentAABB.childCount = static_cast<uint16_t>(end - start);
            parentAABB.u.firstChildIndex = static_cast<int>(AABBTreeVec.size());
            AABBTreeVec.at(parentAABBArrayIndex + parentIdx) = parentAABB;

            for (auto i = start; i < end; i++)
            {
                vec3_t childMins{}, childMaxs{};
                partitionBounds(partitionIndices[i], childMins, childMaxs, true);
                CollisionAabbTree childAABBTree{};
                childAABBTree.materialIndex = material;
                childAABBTree.childCount = 0;
                childAABBTree.u.partitionIndex = partitionIndices[i];
                childAABBTree.origin = CalcMiddleOfAABB(childMins, childMaxs);
                childAABBTree.halfSize = CalcHalfSizeOfAABB(childMins, childMaxs);
                AABBTreeVec.emplace_back(childAABBTree);
            }
        }

        outParentCount = chunks.size();
        outParentStartIndex = parentAABBArrayIndex;
    }

    int ClipMapLinker::LeafTerrainContents(const BSPTree& tree) const
    {
        if (materialContents.empty())
            return BSPEditableConstants::LEAF_TERRAIN_CONTENTS;
        int contents = 0;
        for (size_t objectIdx = 0; objectIdx < tree.leaf->GetObjectCount(); objectIdx++)
        {
            const auto partitionIndex = tree.leaf->GetObject(objectIdx)->partitionIndex;
            const auto material = partitionIndex < static_cast<int>(partitionMaterials.size()) ? partitionMaterials[partitionIndex] : 0;
            contents |= materialContents[material];
        }
        return contents;
    }

    bool ClipMapLinker::LoadClipMaterials(clipMap_t& clipMap)
    {
        // BSP/clipmaterials.json: one clip material per collision FBX material
        // { "materials": [ { "fbx", "name", "contentFlags", "surfaceFlags" } ] }
        const auto path = GetFileNameForBSPAsset("clipmaterials.json");
        const auto file = m_search_path.Open(path);
        if (!file.IsOpen())
        {
            clipMap.info.numMaterials = 1;
            clipMap.info.materials = m_memory.Alloc<ClipMaterial>(1);
            clipMap.info.materials[0].name = m_memory.Dup(BSPLinkingConstants::MISSING_IMAGE_NAME);
            clipMap.info.materials[0].contentFlags = BSPEditableConstants::MATERIAL_CONTENT_FLAGS;
            clipMap.info.materials[0].surfaceFlags = BSPEditableConstants::MATERIAL_SURFACE_FLAGS;
            return true;
        }
        json js;
        try
        {
            js = json::parse(*file.m_stream);
        }
        catch (const json::exception& e)
        {
            con::error("JSON error when parsing {}: {}", path, e.what());
            return false;
        }
        const auto& entries = js.at("materials");
        if (entries.empty() || entries.size() > std::numeric_limits<uint16_t>::max())
        {
            con::error("ERROR: {} has {} clip materials", path, entries.size());
            return false;
        }
        clipMap.info.numMaterials = static_cast<unsigned int>(entries.size());
        clipMap.info.materials = m_memory.Alloc<ClipMaterial>(entries.size());
        for (size_t i = 0; i < entries.size(); i++)
        {
            const auto& entry = entries[i];
            clipMap.info.materials[i].name = m_memory.Dup(entry.at("name").get<std::string>().c_str());
            clipMap.info.materials[i].contentFlags = entry.at("contentFlags").get<int>();
            clipMap.info.materials[i].surfaceFlags = entry.at("surfaceFlags").get<int>();
            materialByFbx[entry.at("fbx").get<std::string>()] = static_cast<uint16_t>(i);
            materialContents.emplace_back(clipMap.info.materials[i].contentFlags);
        }
        con::info("Loaded {} terrain clip materials", entries.size());
        return true;
    }

    constexpr vec3_t normalX = {
        {.x = 1.0f, .y = 0.0f, .z = 0.0f}
    };
    constexpr vec3_t normalY = {
        {.x = 0.0f, .y = 1.0f, .z = 0.0f}
    };
    constexpr vec3_t normalZ = {
        {.x = 0.0f, .y = 0.0f, .z = 1.0f}
    };

    // returns the index of the node/leaf parsed by the function
    // Nodes are indexed by their index in the node array
    // Leafs are indexed by (-1 - <leaf index>)
    // See https://developer.valvesoftware.com/wiki/BSP_(Source)
    int16_t ClipMapLinker::LoadBSPNode(clipMap_t& clipMap, const BSPTree& tree)
    {
        if (tree.isLeaf)
        {
            cLeaf_s leaf;

            leaf.cluster = 0;       // always use cluster 0
            leaf.brushContents = 0; // no brushes used so contents is 0
            // what the leaf's partitions can stop (0 when it has none)
            leaf.terrainContents = tree.leaf->GetObjectCount() > 0 ? LeafTerrainContents(tree) : 0;

            // unused when leafBrushNode == 0
            leaf.mins.x = 0.0f;
            leaf.mins.y = 0.0f;
            leaf.mins.z = 0.0f;
            leaf.maxs.x = 0.0f;
            leaf.maxs.y = 0.0f;
            leaf.maxs.z = 0.0f;
            leaf.leafBrushNode = 0;

            if (tree.leaf->GetObjectCount() > 0)
            {
                size_t parentCount = 0;
                size_t parentStartIndex = 0;
                AddAABBTreeFromLeaf(clipMap, tree, parentCount, parentStartIndex);
                leaf.collAabbCount = static_cast<uint16_t>(parentCount);
                leaf.firstCollAabbIndex = static_cast<uint16_t>(parentStartIndex);
                pendingLeafAabbStart = parentStartIndex;
            }
            else
            {
                leaf.firstCollAabbIndex = 0;
                leaf.collAabbCount = 0;
            }

            uint16_t leafIndex = static_cast<uint16_t>(leafVec.size());
            leafVec.emplace_back(leaf);
            leafFirstAabb.emplace_back(leaf.collAabbCount ? pendingLeafAabbStart : 0);

            return -1 - leafIndex;
        }
        else
        {
            cplane_s plane;
            plane.dist = tree.node->distance;
            if (tree.node->axis == PlaneAxis::AXIS_X)
            {
                plane.normal = normalX;
                plane.type = 0;
            }
            else if (tree.node->axis == PlaneAxis::AXIS_Y)
            {
                plane.normal = normalY;
                plane.type = 1;
            }
            else
            {
                assert(tree.node->axis == PlaneAxis::AXIS_Z);

                plane.normal = normalZ;
                plane.type = 2;
            }

            plane.signbits = 0;
            if (plane.normal.x < 0.0f)
                plane.signbits |= 1;
            if (plane.normal.y < 0.0f)
                plane.signbits |= 2;
            if (plane.normal.z < 0.0f)
                plane.signbits |= 4;

            plane.pad[0] = 0;
            plane.pad[1] = 0;

            planeVec.emplace_back(plane);

            // The recursion of adding the children through LoadBSPNode means the parent node needs to be added before the children are loaded
            size_t nodeIndex = nodeVec.size();
            nodeVec.emplace_back();

            cNode_t node;
            node.plane = nullptr; // initialised after the BSP tree has been loaded
            node.children[0] = LoadBSPNode(clipMap, *tree.node->front);
            node.children[1] = LoadBSPNode(clipMap, *tree.node->back);

            nodeVec.at(nodeIndex) = node;

            return static_cast<uint16_t>(nodeIndex);
        }
    }

    bool ClipMapLinker::LoadBSPTree(clipMap_t& clipMap, const BSPData& bsp)
    {
        vec3_t worldMins;
        vec3_t worldMaxs;
        for (unsigned int vertIdx = 0; vertIdx < clipMap.vertCount; vertIdx++)
        {
            vec3_t vertex = clipMap.verts[vertIdx];
            // initialise AABB with the first vertex
            if (vertIdx == 0)
            {
                worldMins = vertex;
                worldMaxs = vertex;
            }
            UpdateAABBWithPoint(vertex, worldMins, worldMaxs);
        }

        std::vector<std::shared_ptr<BSPObject>> objects;
        for (int partitionIdx = 0; partitionIdx < clipMap.partitionCount; partitionIdx++)
        {
            vec3_t partitionMins;
            vec3_t partitionMaxs;
            auto* partition = &clipMap.partitions[partitionIdx];
            for (int uindIdx = 0; uindIdx < partition->nuinds; uindIdx++)
            {
                uint16_t uind = clipMap.info.uinds[partition->fuind + uindIdx];
                vec3_t vert = clipMap.verts[uind];
                // initalise the AABB with the first vertex
                if (uindIdx == 0)
                {
                    partitionMins = vert;
                    partitionMaxs = vert;
                }
                UpdateAABBWithPoint(vert, partitionMins, partitionMaxs);
            }
            objects.emplace_back(
                std::make_shared<BSPObject>(partitionMins.x, partitionMins.y, partitionMins.z, partitionMaxs.x, partitionMaxs.y, partitionMaxs.z, partitionIdx));
        }
        // The upstream uniform 512-unit grid overflows the int16 node/leaf
        // indices (and the uint16 aabb indices) on large maps; split by content.
        const auto tree = std::make_unique<BSPTree>(worldMins, worldMaxs, std::move(objects), 0);

        // load planes, nodes, leafs, and AABB trees
        LoadBSPNode(clipMap, *tree);

        // Leafs address their parent aabbs with a uint16 index; children are
        // addressed with an int. Put every parent first so the uint16 range
        // only has to hold parents, then each parent's children block.
        {
            std::vector<CollisionAabbTree> reordered;
            std::vector<size_t> newIndex(AABBTreeVec.size(), SIZE_MAX);
            for (size_t i = 0; i < AABBTreeVec.size(); i++)
                if (AABBTreeVec[i].childCount)
                {
                    newIndex[i] = reordered.size();
                    reordered.emplace_back(AABBTreeVec[i]);
                }
            const auto parentCount = reordered.size();
            reordered.reserve(AABBTreeVec.size());
            for (size_t p = 0; p < parentCount; p++)
            {
                // copy out first: appending below may reallocate `reordered`
                const auto first = static_cast<size_t>(reordered[p].u.firstChildIndex);
                const auto childCount = static_cast<size_t>(reordered[p].childCount);
                reordered[p].u.firstChildIndex = static_cast<int>(reordered.size());
                for (size_t c = 0; c < childCount; c++)
                    reordered.emplace_back(AABBTreeVec[first + c]);
            }
            // leafFirstAabb holds the full (pre-truncation) start index of each leaf
            for (size_t l = 0; l < leafVec.size(); l++)
            {
                if (!leafVec[l].collAabbCount)
                    continue;
                const auto index = newIndex[leafFirstAabb[l]];
                if (index > std::numeric_limits<uint16_t>::max())
                {
                    con::error("ERROR: {} parent collision aabbs exceed the uint16 leaf index range", parentCount);
                    return false;
                }
                leafVec[l].firstCollAabbIndex = static_cast<uint16_t>(index);
            }
            AABBTreeVec = std::move(reordered);
        }
        if (nodeVec.size() > static_cast<size_t>(std::numeric_limits<int16_t>::max()) || leafVec.size() > 32768u
)
        {
            con::error("ERROR: collision tree exceeds T6 index limits: {} nodes (max 32767), {} leafs (max 32768), {} aabb trees (max 65535)",
                       nodeVec.size(), leafVec.size(), AABBTreeVec.size());
            return false;
        }

        // brush side planes follow the node planes (node planes share the node's index)
        const auto firstBrushPlane = planeVec.size();
        for (const auto& brush : brushSources)
            planeVec.insert(planeVec.end(), brush.sidePlanes.begin(), brush.sidePlanes.end());

        clipMap.info.planeCount = static_cast<int>(planeVec.size());
        clipMap.info.planes = m_memory.Alloc<cplane_s>(planeVec.size());
        memcpy(clipMap.info.planes, planeVec.data(), sizeof(cplane_s) * planeVec.size());

        clipMap.numNodes = static_cast<unsigned int>(nodeVec.size());
        clipMap.nodes = m_memory.Alloc<cNode_t>(nodeVec.size());
        memcpy(clipMap.nodes, nodeVec.data(), sizeof(cNode_t) * nodeVec.size());

        clipMap.numLeafs = static_cast<unsigned int>(leafVec.size());
        clipMap.leafs = m_memory.Alloc<cLeaf_s>(leafVec.size());
        memcpy(clipMap.leafs, leafVec.data(), sizeof(cLeaf_s) * leafVec.size());

        clipMap.aabbTreeCount = static_cast<unsigned int>(AABBTreeVec.size());
        clipMap.aabbTrees = m_memory.Alloc<CollisionAabbTree>(AABBTreeVec.size());
        memcpy(clipMap.aabbTrees, AABBTreeVec.data(), sizeof(CollisionAabbTree) * AABBTreeVec.size());

        // The plane of each node have the same index
        for (size_t nodeIdx = 0; nodeIdx < nodeVec.size(); nodeIdx++)
            clipMap.nodes[nodeIdx].plane = &clipMap.info.planes[nodeIdx];

        LinkBrushes(clipMap, firstBrushPlane);

        con::info("Collision tree: {} nodes, {} leafs, {} aabb trees, {} partitions, highest leaf object count {}", nodeVec.size(), leafVec.size(),
                  AABBTreeVec.size(), clipMap.partitionCount, highestLeafObjectCount);
        return true;
    }

    bool ClipMapLinker::LoadPartitions(clipMap_t& clipMap, const BSPData& bsp)
    {
        // Collision only needs positions: weld the FBX vertices (which are split by
        // normals/uvs) so shared corners use one clipmap vertex.
        std::vector<vec3_t> weldedVerts;
        std::map<std::tuple<float, float, float>, uint32_t> weldIndex;
        std::vector<uint32_t> remap(bsp.colWorld.vertices.size());
        for (size_t vertIdx = 0; vertIdx < bsp.colWorld.vertices.size(); vertIdx++)
        {
            const auto& pos = bsp.colWorld.vertices[vertIdx].pos;
            const auto [it, inserted] = weldIndex.try_emplace(std::make_tuple(pos.x, pos.y, pos.z), static_cast<uint32_t>(weldedVerts.size()));
            if (inserted)
                weldedVerts.emplace_back(pos);
            remap[vertIdx] = it->second;
        }

        // due to tris using uint16_t as the type for indexing the vert array,
        //  any vertex count over the uint16_t max means the vertices above the uint16_t max can't be indexed
        if (weldedVerts.size() > BSPGameConstants::MAX_COLLISION_VERTS)
        {
            con::error("ERROR: collision vertex count {} exceeds the maximum number: {}!", weldedVerts.size(), BSPGameConstants::MAX_COLLISION_VERTS);
            return false;
        }

        clipMap.vertCount = static_cast<unsigned int>(weldedVerts.size());
        clipMap.verts = m_memory.Alloc<vec3_t>(clipMap.vertCount);
        if (clipMap.vertCount)
            memcpy(clipMap.verts, weldedVerts.data(), sizeof(vec3_t) * clipMap.vertCount);

        // The clipmap index buffer has a unique index for each vertex in the world, compared to the gfxworld's
        //  index buffer having a unique index for each vertex on a surface. This code converts gfxworld indices to clipmap indices.
        // Within each surface, order triangles along a Morton curve of their
        // centroids so consecutive triangles are spatial neighbours: partitions
        // are runs of consecutive triangles, and compact runs keep the number of
        // partitions a query gathers low (the engine keeps only 512 per query).
        std::vector<uint16_t> triIndexVec;
        for (const BSPSurface& surface : bsp.colWorld.surfaces)
        {
            const auto indexOfFirstIndex = surface.indexOfFirstIndex;
            const auto indexOfFirstVertex = surface.indexOfFirstVertex;
            std::vector<std::pair<uint64_t, std::array<uint16_t, 3>>> tris;
            for (auto triIdx = 0u; triIdx < surface.triCount; triIdx++)
            {
                std::array<uint16_t, 3> tri{};
                float centre[3] = {};
                for (auto corner = 0u; corner < 3u; corner++)
                {
                    const auto source = bsp.colWorld.indices[indexOfFirstIndex + triIdx * 3 + corner] + indexOfFirstVertex;
                    tri[corner] = static_cast<uint16_t>(remap[source]);
                    for (auto axis = 0; axis < 3; axis++)
                        centre[axis] += weldedVerts[tri[corner]].v[axis] / 3.0f;
                }
                uint64_t code = 0;
                for (auto bit = 0; bit < 21; bit++)
                    for (auto axis = 0; axis < 3; axis++)
                    {
                        // 32-unit cells, offset so every map coordinate is positive
                        const auto cell = static_cast<uint64_t>(std::clamp((centre[axis] + 65536.0f) / 32.0f, 0.0f, 2097151.0f));
                        code |= ((cell >> bit) & 1ull) << (bit * 3 + axis);
                    }
                tris.emplace_back(code, tri);
            }
            std::ranges::stable_sort(tris, {}, &decltype(tris)::value_type::first);
            for (const auto& [code, tri] : tris)
                triIndexVec.insert(triIndexVec.end(), tri.begin(), tri.end());
        }
        // the reinterpret_cast is used as triIndices is just a pointer to an array of indicies, and static_cast can't safely do the conversion
        clipMap.triCount = static_cast<int>(triIndexVec.size() / 3);
        clipMap.triIndices = reinterpret_cast<uint16_t (*)[3]>(m_memory.Alloc<uint16_t>(triIndexVec.size()));
        memcpy(clipMap.triIndices, triIndexVec.data(), sizeof(uint16_t) * triIndexVec.size());

        // partitions are "containers" for vertices. BSP tree leafs contain a list of these partitions to determine the collision within a leaf.
        // Partitions group runs of consecutive, spatially compact triangles (as
        // compiled maps do). One partition per triangle produces one AABB-tree
        // entry per triangle, which overflows the uint16 leaf aabb indices on
        // large maps. Each partition lists its unique vertices exactly once.
        // Up to 16 triangles per partition, as stock zones (measured: zm_nuked /
        // zm_transit partitions hold 1..16 triangles, mean ~3.6). Traces test
        // every triangle of a partition individually (sub_4F6DB0 -> per-triangle
        // sub_5D1230 / sub_881940); no partition-level box is solid. Movement and
        // missile traces gather at most 512 partitions per query (sub_5C7760),
        // so one triangle per partition dropped collision next to dense meshes
        // (houses, the bus, furniture). The uint16 leaf aabb index limit is met
        // by LoadBSPTree placing all parent aabbs first.
        constexpr int MAX_PARTITION_TRIS = 16;
        constexpr float MAX_PARTITION_EXTENT = 256.0f;
        std::vector<CollisionPartition> partitionVec;
        std::vector<uint16_t> uniqueIndicesVec;
        for (const BSPSurface& surface : bsp.colWorld.surfaces)
        {
            const auto found = materialByFbx.find(surface.material.materialName);
            const uint16_t surfaceMaterial = found != materialByFbx.end() ? found->second : uint16_t{0};
            const auto indexOfFirstTri = static_cast<int>(surface.indexOfFirstIndex / 3);
            auto triIdx = 0;
            while (triIdx < static_cast<int>(surface.triCount))
            {
                CollisionPartition partition;
                partition.firstTri = indexOfFirstTri + triIdx;
                partition.fuind = static_cast<int>(uniqueIndicesVec.size());
                std::vector<uint16_t> unique;
                vec3_t mins{}, maxs{};
                auto count = 0;
                while (count < MAX_PARTITION_TRIS && triIdx + count < static_cast<int>(surface.triCount))
                {
                    const auto* tri = clipMap.triIndices[partition.firstTri + count];
                    auto newMins = count ? mins : clipMap.verts[tri[0]];
                    auto newMaxs = count ? maxs : clipMap.verts[tri[0]];
                    for (auto corner = 0; corner < 3; corner++)
                        UpdateAABBWithPoint(clipMap.verts[tri[corner]], newMins, newMaxs);
                    if (count && (newMaxs.x - newMins.x > MAX_PARTITION_EXTENT || newMaxs.y - newMins.y > MAX_PARTITION_EXTENT
                                  || newMaxs.z - newMins.z > MAX_PARTITION_EXTENT))
                        break;
                    mins = newMins;
                    maxs = newMaxs;
                    for (auto corner = 0; corner < 3; corner++)
                        if (std::find(unique.begin(), unique.end(), tri[corner]) == unique.end())
                            unique.emplace_back(tri[corner]);
                    count++;
                }
                partition.triCount = static_cast<char>(count);
                partition.nuinds = static_cast<int>(unique.size());
                uniqueIndicesVec.insert(uniqueIndicesVec.end(), unique.begin(), unique.end());
                partitionVec.emplace_back(partition);
                partitionMaterials.emplace_back(surfaceMaterial);
                triIdx += count;
            }
        }
        clipMap.partitionCount = static_cast<int>(partitionVec.size());
        clipMap.partitions = m_memory.Alloc<CollisionPartition>(clipMap.partitionCount);
        memcpy(clipMap.partitions, partitionVec.data(), sizeof(CollisionPartition) * partitionVec.size());

        clipMap.info.nuinds = static_cast<int>(uniqueIndicesVec.size());
        clipMap.info.uinds = m_memory.Alloc<uint16_t>(uniqueIndicesVec.size());
        memcpy(clipMap.info.uinds, uniqueIndicesVec.data(), sizeof(uint16_t) * uniqueIndicesVec.size());

        return true;

        /*
        // Proper unique index creation code kept for future use
        int totalUindCount = 0;
        std::vector<uint16_t> uindVec;
        for (int i = 0; i < clipMap->partitionCount; i++)
        {
            CollisionPartition* currPartition = &clipMap->partitions[i];
            std::vector<uint16_t> uniqueVertVec;
            for (int k = 0; k < currPartition->triCount; k++)
            {
                uint16_t* tri = clipMap->triIndices[currPartition->firstTri + k];
                for (int l = 0; l < 3; l++)
                {
                    bool isVertexIndexUnique = true;
                    uint16_t vertIndex = tri[l];

                    for (size_t m = 0; m < uniqueVertVec.size(); m++)
                    {
                        if (uniqueVertVec[m] == vertIndex)
                        {
                            isVertexIndexUnique = false;
                            break;
                        }
                    }

                    if (isVertexIndexUnique)
                        uniqueVertVec.emplace_back(vertIndex);
                }
            }

            currPartition->fuind = totalUindCount;
            currPartition->nuinds = (int)uniqueVertVec.size();
            uindVec.insert(uindVec.end(), uniqueVertVec.begin(), uniqueVertVec.end());
            totalUindCount += currPartition->nuinds;
        }
        clipMap->info.nuinds = totalUindCount;
        clipMap->info.uinds = m_memory.Alloc<uint16_t>(totalUindCount);
        memcpy(clipMap->info.uinds, &uindVec[0], sizeof(uint16_t) * totalUindCount);
        */
    }

    namespace
    {
        ClipMapLinker::BrushSource ParseBrush(const json& brushJs)
        {
            ClipMapLinker::BrushSource brush{};
            const auto& mins = brushJs.at("mins");
            const auto& maxs = brushJs.at("maxs");
            brush.mins = {{.x = mins.at(0).get<float>(), .y = mins.at(1).get<float>(), .z = mins.at(2).get<float>()}};
            brush.maxs = {{.x = maxs.at(0).get<float>(), .y = maxs.at(1).get<float>(), .z = maxs.at(2).get<float>()}};
            brush.contents = brushJs.at("contents").get<int>();

            const auto& axial = brushJs.at("axial");
            for (auto axialIdx = 0u; axialIdx < 6u; axialIdx++)
            {
                brush.axialCflags[axialIdx % 2][axialIdx / 2] = axial.at(axialIdx).at(0).get<int>();
                brush.axialSflags[axialIdx % 2][axialIdx / 2] = axial.at(axialIdx).at(1).get<int>();
            }

            for (const auto& sideJs : brushJs.at("sides"))
            {
                cplane_s plane{};
                plane.normal.x = sideJs.at(0).get<float>();
                plane.normal.y = sideJs.at(1).get<float>();
                plane.normal.z = sideJs.at(2).get<float>();
                plane.dist = sideJs.at(3).get<float>();
                if (plane.normal.x == 1.0f && plane.normal.y == 0.0f && plane.normal.z == 0.0f)
                    plane.type = 0;
                else if (plane.normal.x == 0.0f && plane.normal.y == 1.0f && plane.normal.z == 0.0f)
                    plane.type = 1;
                else if (plane.normal.x == 0.0f && plane.normal.y == 0.0f && plane.normal.z == 1.0f)
                    plane.type = 2;
                else
                    plane.type = 3;
                plane.signbits = static_cast<char>((plane.normal.x < 0.0f ? 1 : 0) | (plane.normal.y < 0.0f ? 2 : 0) | (plane.normal.z < 0.0f ? 4 : 0));
                brush.sidePlanes.emplace_back(plane);
                brush.sideFlags.emplace_back(sideJs.at(4).get<int>(), sideJs.at(5).get<int>());
            }

            for (const auto& vertJs : brushJs.at("verts"))
                brush.verts.push_back({{.x = vertJs.at(0).get<float>(), .y = vertJs.at(1).get<float>(), .z = vertJs.at(2).get<float>()}});
            return brush;
        }
    } // namespace

    bool ClipMapLinker::ReadBrushFile()
    {
        // BSP/brushes.json, game coordinates:
        // { "brushes": [ { "mins": [3], "maxs": [3], "contents": int,
        //                  "axial": [[cflags, sflags] x6 in -x,+x,-y,+y,-z,+z order],
        //                  "sides": [[nx, ny, nz, dist, cflags, sflags], ...],
        //                  "verts": [[x, y, z], ...] } ] }
        const auto brushFilePath = GetFileNameForBSPAsset("brushes.json");
        const auto brushFile = m_search_path.Open(brushFilePath);
        if (!brushFile.IsOpen())
        {
            con::warn("Can't find brush file {}, the map will only have terrain collision", brushFilePath);
            return true;
        }

        try
        {
            const auto js = json::parse(*brushFile.m_stream);
            for (const auto& brushJs : js.at("brushes"))
                brushSources.emplace_back(ParseBrush(brushJs));
            worldBrushCount = brushSources.size();

            // BSP/submodels.json: brush models *1..*N in entity order
            // { "submodels": [ { "mins": [3], "maxs": [3], "brushes": [ <brush>, ... ] } ] }
            const auto subFile = m_search_path.Open(GetFileNameForBSPAsset("submodels.json"));
            if (subFile.IsOpen())
            {
                const auto subJs = json::parse(*subFile.m_stream);
                for (const auto& modelJs : subJs.at("submodels"))
                {
                    SubModelSource sub{};
                    const auto& mins = modelJs.at("mins");
                    const auto& maxs = modelJs.at("maxs");
                    sub.mins = {{.x = mins.at(0).get<float>(), .y = mins.at(1).get<float>(), .z = mins.at(2).get<float>()}};
                    sub.maxs = {{.x = maxs.at(0).get<float>(), .y = maxs.at(1).get<float>(), .z = maxs.at(2).get<float>()}};
                    sub.firstBrush = brushSources.size();
                    for (const auto& brushJs : modelJs.at("brushes"))
                        brushSources.emplace_back(ParseBrush(brushJs));
                    sub.brushCount = brushSources.size() - sub.firstBrush;
                    subModelSources.emplace_back(sub);
                }
            }
        }
        catch (const json::exception& e)
        {
            con::error("JSON error when parsing {}: {}", brushFilePath, e.what());
            return false;
        }

        if (brushSources.size() > std::numeric_limits<uint16_t>::max())
        {
            con::error("ERROR: {} brushes exceed the limit of {}", brushSources.size(), std::numeric_limits<uint16_t>::max());
            return false;
        }
        con::info("Loaded {} collision brushes ({} world, {} submodels)", brushSources.size(), worldBrushCount, subModelSources.size());
        return true;
    }

    // Builds the leaf brush kd-tree the same way the IW compilers lay it out:
    //  - a node with leafBrushCount > 0 is a leaf listing brush indices
    //  - negative count visits the immediately following subtree for crossing brushes
    //  - childOffset[0/1] hold brushes entirely in front of / behind the split
    // Position traces ignore range; crossing brushes must never be assigned by centre.
    uint16_t ClipMapLinker::BuildLeafBrushNode(std::vector<uint16_t>& brushIds,
                                               std::vector<cLeafBrushNode_s>& nodes,
                                               std::vector<uint16_t>& leafBrushes,
                                               std::vector<size_t>& leafOffsets) const
    {
        constexpr size_t MAX_LEAF_BRUSHES = 8;

        const auto index = nodes.size();
        nodes.emplace_back();
        leafOffsets.emplace_back(std::numeric_limits<size_t>::max());

        int contents = 0;
        float centreMins[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float centreMaxs[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        const auto centre = [this](const uint16_t id, const int axis)
        {
            const auto& brush = brushSources[id];
            return (brush.mins.v[axis] + brush.maxs.v[axis]) * 0.5f;
        };
        for (const auto id : brushIds)
        {
            contents |= brushSources[id].contents;
            for (auto axis = 0; axis < 3; axis++)
            {
                centreMins[axis] = std::min(centreMins[axis], centre(id, axis));
                centreMaxs[axis] = std::max(centreMaxs[axis], centre(id, axis));
            }
        }

        auto axis = 0;
        for (auto a = 1; a < 3; a++)
            if (centreMaxs[a] - centreMins[a] > centreMaxs[axis] - centreMins[axis])
                axis = a;

        const auto makeLeaf = [&]()
        {
            auto& node = nodes[index];
            node.axis = 0;
            node.leafBrushCount = static_cast<int16_t>(brushIds.size());
            node.contents = contents;
            leafOffsets[index] = leafBrushes.size();
            leafBrushes.insert(leafBrushes.end(), brushIds.begin(), brushIds.end());
            return static_cast<uint16_t>(index);
        };

        if (brushIds.size() <= MAX_LEAF_BRUSHES || centreMaxs[axis] - centreMins[axis] <= 0.0f)
            return makeLeaf();

        std::ranges::sort(brushIds, [&](const uint16_t a, const uint16_t b) { return centre(a, axis) < centre(b, axis); });
        const auto mid = brushIds.size() / 2;
        const auto dist = centre(brushIds[mid], axis);
        std::vector<uint16_t> front, back, crossing;
        for (const auto id : brushIds)
        {
            const auto& brush = brushSources[id];
            if (brush.mins.v[axis] > dist)
                front.push_back(id);
            else if (brush.maxs.v[axis] < dist)
                back.push_back(id);
            else
                crossing.push_back(id);
        }
        // Point-contents traversal has no contents early-out: an empty split
        // with zero child offsets loops forever. Both branches must be real.
        if (front.empty() || back.empty())
            return makeLeaf();
        // Native traversal always visits index + 1 when leafBrushCount is negative.
        if (!crossing.empty())
            BuildLeafBrushNode(crossing, nodes, leafBrushes, leafOffsets);

        const auto frontIndex = BuildLeafBrushNode(front, nodes, leafBrushes, leafOffsets);
        const auto backIndex = BuildLeafBrushNode(back, nodes, leafBrushes, leafOffsets);

        auto& node = nodes[index];
        node.axis = static_cast<char>(axis);
        node.leafBrushCount = crossing.empty() ? 0 : -1;
        node.contents = contents;
        node.data.children.dist = dist;
        node.data.children.range = 0.0f;
        node.data.children.childOffset[0] = static_cast<uint16_t>(frontIndex - index);
        node.data.children.childOffset[1] = static_cast<uint16_t>(backIndex - index);
        return static_cast<uint16_t>(index);
    }

    void ClipMapLinker::LinkBrushes(clipMap_t& clipMap, const size_t firstBrushPlane)
    {
        const auto brushCount = brushSources.size();
        if (brushCount == 0)
            return;

        size_t sideCount = 0;
        size_t vertCount = 0;
        for (const auto& brush : brushSources)
        {
            sideCount += brush.sidePlanes.size();
            vertCount += brush.verts.size();
        }

        clipMap.info.numBrushes = static_cast<uint16_t>(brushCount);
        clipMap.info.brushes = m_memory.Alloc<cbrush_t>(brushCount);
        // stock zones leave these null (the game derives them); match them
        clipMap.info.brushBounds = nullptr;
        clipMap.info.brushContents = nullptr;
        clipMap.info.numBrushSides = static_cast<unsigned int>(sideCount);
        clipMap.info.brushsides = m_memory.Alloc<cbrushside_t>(sideCount);
        clipMap.info.numBrushVerts = static_cast<unsigned int>(vertCount);
        clipMap.info.brushVerts = m_memory.Alloc<vec3_t>(vertCount);

        vec3_t allMins = brushSources[0].mins;
        vec3_t allMaxs = brushSources[0].maxs;
        size_t sideIdx = 0;
        size_t vertIdx = 0;
        for (size_t brushIdx = 0; brushIdx < brushCount; brushIdx++)
        {
            const auto& source = brushSources[brushIdx];
            auto& brush = clipMap.info.brushes[brushIdx];
            brush.mins = source.mins;
            brush.maxs = source.maxs;
            brush.contents = source.contents;
            memcpy(brush.axial_cflags, source.axialCflags, sizeof(brush.axial_cflags));
            memcpy(brush.axial_sflags, source.axialSflags, sizeof(brush.axial_sflags));

            brush.numsides = static_cast<unsigned int>(source.sidePlanes.size());
            brush.sides = brush.numsides ? &clipMap.info.brushsides[sideIdx] : nullptr;
            for (size_t i = 0; i < source.sidePlanes.size(); i++, sideIdx++)
            {
                clipMap.info.brushsides[sideIdx].plane = &clipMap.info.planes[firstBrushPlane + sideIdx];
                clipMap.info.brushsides[sideIdx].cflags = source.sideFlags[i].first;
                clipMap.info.brushsides[sideIdx].sflags = source.sideFlags[i].second;
            }

            brush.numverts = static_cast<unsigned int>(source.verts.size());
            brush.verts = brush.numverts ? &clipMap.info.brushVerts[vertIdx] : nullptr;
            for (const auto& vert : source.verts)
                clipMap.info.brushVerts[vertIdx++] = vert;

            UpdateAABB(source.mins, source.maxs, allMins, allMaxs);
        }

        // node 0 is an empty node, as in compiled maps
        std::vector<cLeafBrushNode_s> nodes(1);
        std::vector<size_t> leafOffsets(1, std::numeric_limits<size_t>::max());
        std::vector<uint16_t> leafBrushes;
        // the world tree holds only world brushes; each brush model gets its own subtree
        std::vector<uint16_t> worldIds(worldBrushCount);
        for (size_t i = 0; i < worldBrushCount; i++)
            worldIds[i] = static_cast<uint16_t>(i);
        const auto root = worldIds.empty() ? static_cast<uint16_t>(0) : BuildLeafBrushNode(worldIds, nodes, leafBrushes, leafOffsets);
        std::vector<uint16_t> subRoots(subModelSources.size(), 0);
        for (size_t subIdx = 0; subIdx < subModelSources.size(); subIdx++)
        {
            const auto& sub = subModelSources[subIdx];
            if (!sub.brushCount)
                continue;
            std::vector<uint16_t> ids(sub.brushCount);
            for (size_t i = 0; i < sub.brushCount; i++)
                ids[i] = static_cast<uint16_t>(sub.firstBrush + i);
            subRoots[subIdx] = BuildLeafBrushNode(ids, nodes, leafBrushes, leafOffsets);
        }
        assert(nodes.size() <= std::numeric_limits<uint16_t>::max());

        clipMap.info.numLeafBrushes = static_cast<unsigned int>(leafBrushes.size());
        clipMap.info.leafbrushes = m_memory.Alloc<LeafBrush>(leafBrushes.size());
        memcpy(clipMap.info.leafbrushes, leafBrushes.data(), sizeof(LeafBrush) * leafBrushes.size());
        clipMap.info.leafbrushNodesCount = static_cast<unsigned int>(nodes.size());
        clipMap.info.leafbrushNodes = m_memory.Alloc<cLeafBrushNode_s>(nodes.size());
        for (size_t i = 0; i < nodes.size(); i++)
        {
            clipMap.info.leafbrushNodes[i] = nodes[i];
            if (leafOffsets[i] != std::numeric_limits<size_t>::max())
                clipMap.info.leafbrushNodes[i].data.leaf.brushes = &clipMap.info.leafbrushes[leafOffsets[i]];
        }

        // Every collision leaf sees the whole brush tree; the tree does the spatial culling.
        const auto rootContents = nodes[root].contents;
        clipMap.cmodels[0].leaf.leafBrushNode = root;
        clipMap.cmodels[0].leaf.brushContents = rootContents;
        clipMap.cmodels[0].leaf.mins = allMins;
        clipMap.cmodels[0].leaf.maxs = allMaxs;
        for (unsigned int leafIdx = 0; leafIdx < clipMap.numLeafs; leafIdx++)
        {
            auto& leaf = clipMap.leafs[leafIdx];
            leaf.leafBrushNode = root;
            leaf.brushContents = rootContents;
            leaf.mins = allMins;
            leaf.maxs = allMaxs;
        }
        // brush models *1..*N (doors, blockers, triggers, zone volumes)
        const auto modelCount = 1u + static_cast<unsigned int>(subModelSources.size());
        auto* cmodels = m_memory.Alloc<cmodel_t>(modelCount);
        cmodels[0] = clipMap.cmodels[0];
        // as in stock zones the world model's own leaf is empty; world traces walk the node tree
        cmodels[0].leaf = {};
        for (size_t subIdx = 0; subIdx < subModelSources.size(); subIdx++)
        {
            const auto& sub = subModelSources[subIdx];
            auto& model = cmodels[subIdx + 1];
            model.mins = sub.mins;
            model.maxs = sub.maxs;
            model.radius = DistBetweenPoints(sub.mins, sub.maxs) / 2;
            model.info = nullptr;
            model.leaf.firstCollAabbIndex = 0;
            model.leaf.collAabbCount = 0;
            model.leaf.terrainContents = 0;
            model.leaf.cluster = 0;
            model.leaf.mins = sub.mins;
            model.leaf.maxs = sub.maxs;
            model.leaf.leafBrushNode = subRoots[subIdx];
            model.leaf.brushContents = sub.brushCount ? nodes[subRoots[subIdx]].contents : 0;
        }
        clipMap.numSubModels = modelCount;
        clipMap.cmodels = cmodels;

        con::info("Linked {} brushes, {} sides, {} leaf brush nodes, {} brush models", brushCount, sideCount, nodes.size(), modelCount);
    }

    bool ClipMapLinker::LoadWorldCollision(clipMap_t& clipMap, const BSPData& bsp)
    {
        if (!ReadBrushFile())
            return false;

        // brush data is filled in by LinkBrushes once the planes exist
        clipMap.info.numBrushSides = 0;
        clipMap.info.brushsides = nullptr;
        clipMap.info.leafbrushNodesCount = 0;
        clipMap.info.leafbrushNodes = nullptr;
        clipMap.info.numLeafBrushes = 0;
        clipMap.info.leafbrushes = nullptr;
        clipMap.info.numBrushVerts = 0;
        clipMap.info.brushVerts = nullptr;
        clipMap.info.numBrushes = 0;
        clipMap.info.brushes = nullptr;
        clipMap.info.brushBounds = nullptr;
        clipMap.info.brushContents = nullptr;

        // load verts, tris, uinds and partitions
        if (!LoadPartitions(clipMap, bsp))
            return false;

        if (!LoadBSPTree(clipMap, bsp))
            return false;

        return true;
    }

    clipMap_t* ClipMapLinker::LinkClipMap(const BSPData& bsp)
    {
        clipMap_t* clipMap = m_memory.Alloc<clipMap_t>();
        clipMap->name = m_memory.Dup(bsp.bspName.c_str());

        // stock zones ship 0; the game sets it when the clipmap is loaded
        clipMap->isInUse = false;
        clipMap->checksum = 0;
        clipMap->pInfo = nullptr;

        std::string mapEntsName = bsp.bspName;
        auto mapEntsAsset = m_context.LoadDependency<AssetMapEnts>(mapEntsName);
        assert(mapEntsAsset != nullptr);
        clipMap->mapEnts = mapEntsAsset->Asset();

        LoadBoxData(*clipMap);

        LoadVisibility(*clipMap);

        LoadRopesAndConstraints(*clipMap);

        LoadSubModelCollision(*clipMap, bsp);

        LoadDynEnts(*clipMap);

        if (!LoadXModelCollision(*clipMap))
            return nullptr;

        // Clip materials: one per WaW collision material (BSP/clipmaterials.json), so
        // missile/shot clip, player clip and glass keep their own contents.
        if (!LoadClipMaterials(*clipMap))
            return nullptr;

        if (!LoadWorldCollision(*clipMap, bsp))
            return nullptr;

        // set all edges to walkable (one bit per triangle edge)
        // Must follow LoadWorldCollision: triCount is only known after the
        // partitions are loaded. Sizing it before gave a 0-byte buffer that the
        // zone writer and the game then read as 3 bits per triangle.
        int walkableEdgeSize = (3 * clipMap->triCount + 31) / 32 * 4;
        clipMap->triEdgeIsWalkable = m_memory.Alloc<char>(walkableEdgeSize);
        memset(clipMap->triEdgeIsWalkable, 0xFF, walkableEdgeSize * sizeof(char));

        return clipMap;
    }
} // namespace BSP

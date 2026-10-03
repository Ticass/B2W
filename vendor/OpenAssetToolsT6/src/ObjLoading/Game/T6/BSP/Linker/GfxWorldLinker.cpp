#include "GfxWorldLinker.h"

#include "Game/T6/BSP/BSPUtil.h"
#include "Utils/Alignment.h"
#include "Utils/Pack.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <limits>
#include <nlohmann/json.hpp>

using namespace nlohmann;

using namespace T6;

namespace
{
    // Layout of the compiled cell AABB trees (verified against zm_nuked):
    //  - children of a node are contiguous; childrenOffset is the byte offset
    //    from the node to its first child
    //  - every node covers the contiguous range [startSurfIndex, +surfaceCount)
    //    of dpvs.sortedSurfIndex for its whole subtree, and lists every static
    //    model of its subtree in smodelIndexes
    //  - leafs hold at most ~100 surfaces and a handful of static models
    struct AabbItem
    {
        vec3_t mins;
        vec3_t maxs;
        bool isModel;
        uint16_t index;
    };

    struct AabbBuildNode
    {
        vec3_t mins;
        vec3_t maxs;
        std::vector<AabbItem> items;
        std::vector<std::unique_ptr<AabbBuildNode>> children;
    };

    constexpr size_t MAX_LEAF_SURFACES = 64;
    constexpr size_t MAX_LEAF_MODELS = 12;

    std::unique_ptr<AabbBuildNode> BuildAabbNode(std::vector<AabbItem> items, const int depth)
    {
        auto node = std::make_unique<AabbBuildNode>();
        node->mins = items[0].mins;
        node->maxs = items[0].maxs;
        size_t surfaces = 0, models = 0;
        for (const auto& item : items)
        {
            BSP::UpdateAABB(item.mins, item.maxs, node->mins, node->maxs);
            (item.isModel ? models : surfaces)++;
        }
        if ((surfaces <= MAX_LEAF_SURFACES && models <= MAX_LEAF_MODELS) || items.size() < 2 || depth > 40)
        {
            node->items = std::move(items);
            return node;
        }

        auto axis = 0;
        for (auto a = 1; a < 3; a++)
            if (node->maxs.v[a] - node->mins.v[a] > node->maxs.v[axis] - node->mins.v[axis])
                axis = a;
        const auto mid = items.size() / 2;
        std::ranges::nth_element(items, items.begin() + static_cast<std::ptrdiff_t>(mid),
                                 [axis](const AabbItem& a, const AabbItem& b)
                                 { return a.mins.v[axis] + a.maxs.v[axis] < b.mins.v[axis] + b.maxs.v[axis]; });
        std::vector<AabbItem> back(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(mid));
        std::vector<AabbItem> front(items.begin() + static_cast<std::ptrdiff_t>(mid), items.end());
        node->children.emplace_back(BuildAabbNode(std::move(back), depth + 1));
        node->children.emplace_back(BuildAabbNode(std::move(front), depth + 1));
        return node;
    }

    // leaf-order surface list, so every subtree covers a contiguous range
    void CollectSurfaces(const AabbBuildNode& node, std::vector<uint16_t>& sorted)
    {
        for (const auto& item : node.items)
            if (!item.isModel)
                sorted.emplace_back(item.index);
        for (const auto& child : node.children)
            CollectSurfaces(*child, sorted);
    }

    void CollectModels(const AabbBuildNode& node, std::vector<uint16_t>& models)
    {
        for (const auto& item : node.items)
            if (item.isModel)
                models.emplace_back(item.index);
        for (const auto& child : node.children)
            CollectModels(*child, models);
    }

    size_t CountSurfaces(const AabbBuildNode& node)
    {
        auto count = static_cast<size_t>(std::ranges::count_if(node.items, [](const AabbItem& i) { return !i.isModel; }));
        for (const auto& child : node.children)
            count += CountSurfaces(*child);
        return count;
    }

    void BuildCellAabbTree(MemoryManager& memory, GfxWorld& gfxWorld, GfxCell& cell)
    {
        std::vector<AabbItem> items;
        for (auto i = 0u; i < gfxWorld.dpvs.staticSurfaceCount; i++)
            items.push_back({gfxWorld.dpvs.surfaces[i].bounds[0], gfxWorld.dpvs.surfaces[i].bounds[1], false, static_cast<uint16_t>(i)});
        for (auto i = 0u; i < gfxWorld.dpvs.smodelCount; i++)
            items.push_back({gfxWorld.dpvs.smodelInsts[i].mins, gfxWorld.dpvs.smodelInsts[i].maxs, true, static_cast<uint16_t>(i)});
        if (items.empty())
        {
            cell.aabbTreeCount = 0;
            cell.aabbTree = nullptr;
            return;
        }
        const auto root = BuildAabbNode(std::move(items), 0);

        // sortedSurfIndex in leaf order; each subtree is a contiguous range
        std::vector<uint16_t> sorted;
        CollectSurfaces(*root, sorted);
        for (size_t i = 0; i < sorted.size(); i++)
            gfxWorld.dpvs.sortedSurfIndex[i] = sorted[i];

        // flatten breadth first so siblings are contiguous
        struct Pending
        {
            const AabbBuildNode* node;
            size_t slot;
            size_t surfStart;
        };
        std::vector<const AabbBuildNode*> flat{root.get()};
        std::vector<size_t> surfStarts{0};
        std::vector<Pending> queue{{root.get(), 0, 0}};
        std::vector<std::pair<size_t, size_t>> childRange(1, {0, 0}); // first child slot, count
        for (size_t q = 0; q < queue.size(); q++)
        {
            const auto [node, slot, surfStart] = queue[q];
            auto childSurfStart = surfStart + static_cast<size_t>(std::ranges::count_if(node->items, [](const AabbItem& i) { return !i.isModel; }));
            childRange[slot] = {flat.size(), node->children.size()};
            for (const auto& child : node->children)
            {
                const auto childSlot = flat.size();
                flat.emplace_back(child.get());
                surfStarts.emplace_back(childSurfStart);
                childRange.emplace_back(0, 0);
                queue.push_back({child.get(), childSlot, childSurfStart});
                childSurfStart += CountSurfaces(*child);
            }
        }

        cell.aabbTreeCount = static_cast<int>(flat.size());
        cell.aabbTree = memory.Alloc<GfxAabbTree>(flat.size());
        auto leaves = 0;
        for (size_t i = 0; i < flat.size(); i++)
        {
            auto& out = cell.aabbTree[i];
            const auto* node = flat[i];
            out.mins = node->mins;
            out.maxs = node->maxs;
            out.childCount = static_cast<uint16_t>(childRange[i].second);
            out.childrenOffset = childRange[i].second ? static_cast<int>((childRange[i].first - i) * sizeof(GfxAabbTree)) : 0;
            out.startSurfIndex = static_cast<uint16_t>(surfStarts[i]);
            out.surfaceCount = static_cast<uint16_t>(CountSurfaces(*node));
            std::vector<uint16_t> models;
            CollectModels(*node, models);
            out.smodelIndexCount = static_cast<uint16_t>(models.size());
            out.smodelIndexes = models.empty() ? nullptr : memory.Alloc<uint16_t>(models.size());
            if (!models.empty())
                std::memcpy(out.smodelIndexes, models.data(), models.size() * sizeof(uint16_t));
            leaves += out.childCount == 0;
        }
        con::info("Cell AABB tree: {} nodes, {} leaves", flat.size(), leaves);
    }
} // namespace

namespace
{
    // BSP/lightmaps.json written by waw2bo2.lightmaps:
    // { "pages": [ { "waw": image, "t6": image } ], "wawMaterials": [ material, ... ] }
    struct LightmapPlan
    {
        std::vector<std::pair<std::string, std::string>> pages; // WaW-encoded, T6-encoded
        std::set<std::string> wawMaterials;
    };

    LightmapPlan ReadLightmapPlan(ISearchPath& searchPath)
    {
        LightmapPlan plan;
        const auto path = BSP::GetFileNameForBSPAsset("lightmaps.json");
        const auto file = searchPath.Open(path);
        if (!file.IsOpen())
            return plan;
        try
        {
            const auto js = nlohmann::json::parse(*file.m_stream);
            for (const auto& page : js.at("pages"))
                plan.pages.emplace_back(page.at("waw").get<std::string>(), page.at("t6").get<std::string>());
            for (const auto& name : js.at("wawMaterials"))
                plan.wawMaterials.emplace(name.get<std::string>());
        }
        catch (const nlohmann::json::exception& e)
        {
            con::error("JSON error when parsing {}: {}", path, e.what());
            plan = {};
        }
        return plan;
    }

    // WaW programs read the WaW-encoded copy of a page, T6 donor programs the T6 one.
    unsigned char SurfaceLightmap(const LightmapPlan& plan, const BSP::BSPSurface& surface)
    {
        if (plan.pages.empty() || surface.lightmapPage < 0 || static_cast<size_t>(surface.lightmapPage) >= plan.pages.size())
            return BSP::BSPEditableConstants::DEFAULT_SURFACE_LIGHTMAP;
        const auto waw = plan.wawMaterials.contains(surface.material.materialName);
        return static_cast<unsigned char>(waw ? surface.lightmapPage : plan.pages.size() + surface.lightmapPage);
    }
} // namespace

namespace BSP
{
    GfxWorldLinker::GfxWorldLinker(MemoryManager& memory, ISearchPath& searchPath, AssetCreationContext& context)
        : m_memory(memory),
          m_search_path(searchPath),
          m_context(context)
    {
    }

    void GfxWorldLinker::LoadDrawData(const BSPData& bsp, GfxWorld& gfxWorld) const
    {
        size_t vertexCount = bsp.gfxWorld.vertices.size();
        gfxWorld.draw.vertexCount = static_cast<unsigned int>(vertexCount);
        gfxWorld.draw.vertexDataSize0 = static_cast<unsigned int>(vertexCount * sizeof(GfxPackedWorldVertex));
        GfxPackedWorldVertex* vertexBuffer = m_memory.Alloc<GfxPackedWorldVertex>(vertexCount);
        for (size_t vertIdx = 0; vertIdx < vertexCount; vertIdx++)
        {
            const BSPVertex& bspVertex = bsp.gfxWorld.vertices.at(vertIdx);
            GfxPackedWorldVertex* gfxVertex = &vertexBuffer[vertIdx];

            gfxVertex->xyz = bspVertex.pos;
            gfxVertex->color.packed = pack32::Vec4PackGfxColor(bspVertex.color.v);
            gfxVertex->texCoord.packed = pack32::Vec2PackTexCoordsUV(bspVertex.texCoord.v);
            gfxVertex->normal.packed = pack32::Vec3PackUnitVecThirdBased(bspVertex.normal.v);
            gfxVertex->tangent.packed = pack32::Vec3PackUnitVecThirdBased(bspVertex.tangent.v);

            gfxVertex->binormalSign = bspVertex.binormalSign;
            // source lightmap UV, streamed to lit world passes as TEXCOORD1
            gfxVertex->lmapCoord.packed = pack32::Vec2PackTexCoordsUV(bspVertex.lmapCoord.v);
        }
        gfxWorld.draw.vd0.data = reinterpret_cast<char*>(vertexBuffer);

        // vd1 is unused but still needs to be initialised
        // the data type varies and 0x20 is enough for all types
        gfxWorld.draw.vertexDataSize1 = 0x20;
        gfxWorld.draw.vd1.data = m_memory.Alloc<char>(gfxWorld.draw.vertexDataSize1);

        size_t indexCount = bsp.gfxWorld.indices.size();
        assert(indexCount % 3 == 0);
        gfxWorld.draw.indexCount = static_cast<int>(indexCount);
        gfxWorld.draw.indices = m_memory.Alloc<uint16_t>(indexCount);
        for (size_t indexIdx = 0; indexIdx < indexCount; indexIdx++)
        {
            gfxWorld.draw.indices[indexIdx] = bsp.gfxWorld.indices.at(indexIdx);
        }
    }

    void GfxWorldLinker::AppendLayerVertices(
        const BSPData& bsp, const BSPSurface& bspSurface, GfxSurface& gfxSurface, GfxWorld& gfxWorld, std::vector<char>& layerData) const
    {
        // MTL_WORLDVERT_TEX_<t>_NRM_<n> (same enum in WaW and T6): per vertex,
        // t-1 half2 texcoords then n-1 packed normal transforms, read through
        // the techniques' TEXCOORD_2.. / NORMAL_TRANSFORM_.. streams (layout
        // measured on stock zm_nuked vd1 data).
        static constexpr unsigned char FORMAT_LAYERS[][2] = {
            {1, 1}, {2, 1}, {2, 2}, {3, 1}, {3, 2}, {3, 3}, {4, 1}, {4, 2}, {4, 3}, {5, 1}, {5, 2}, {5, 3}};
        const auto* techset = gfxSurface.material ? gfxSurface.material->techniqueSet : nullptr;
        const auto format = techset ? static_cast<unsigned char>(techset->worldVertFormat) : 0u;
        const auto* vertices = &bsp.gfxWorld.vertices[bspSurface.indexOfFirstVertex];
        const auto count = static_cast<size_t>(gfxSurface.tris.vertexCount);
        const bool sourceLayers = count && vertices[0].layerTexCoordCount > 0;
        if (format == 0 || format >= std::size(FORMAT_LAYERS))
        {
            if (sourceLayers)
            {
                // a layered source drawn by a single-layer technique: its vertex
                // colour holds layer blend weights, which must not tint the base
                auto* packed = reinterpret_cast<GfxPackedWorldVertex*>(&gfxWorld.draw.vd0.data[gfxSurface.tris.vertexDataOffset0]);
                const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                for (size_t i = 0; i < count; ++i)
                    packed[i].color.packed = pack32::Vec4PackGfxColor(white);
            }
            return;
        }
        const auto texcoords = FORMAT_LAYERS[format][0] - 1u;
        const auto normals = FORMAT_LAYERS[format][1] - 1u;
        if (!sourceLayers || vertices[0].layerTexCoordCount < texcoords)
            con::warn("surface material {} needs {} layer texcoords, the source has {}; missing layers read (0, 0)",
                      gfxSurface.material->info.name, texcoords, count ? vertices[0].layerTexCoordCount : 0);
        gfxSurface.tris.vertexDataOffset1 = static_cast<int>(layerData.size());
        for (size_t i = 0; i < count; ++i)
        {
            const auto& vertex = vertices[i];
            for (unsigned k = 0; k < texcoords; ++k)
            {
                const float uv[2] = {vertex.layerTexCoords[k].x, vertex.layerTexCoords[k].y};
                const auto packed = pack32::Vec2PackTexCoordsUV(uv);
                layerData.insert(layerData.end(), reinterpret_cast<const char*>(&packed), reinterpret_cast<const char*>(&packed) + 4);
            }
            for (unsigned k = 0; k < normals; ++k)
            {
                // WaW stores the transform RGBA, T6 reads it as B8G8R8A8: swap
                // bytes 0 and 2 so the program sees the same values. A layer
                // normal map the source lacks gets the identity rotation.
                const uint32_t waw = k < vertex.layerNormalCount ? vertex.layerNormals[k] : 0xFF8080FFu;
                const char bytes[4] = {static_cast<char>((waw >> 16) & 0xFF), static_cast<char>((waw >> 8) & 0xFF),
                                       static_cast<char>(waw & 0xFF), static_cast<char>((waw >> 24) & 0xFF)};
                layerData.insert(layerData.end(), bytes, bytes + 4);
            }
        }
    }

    bool GfxWorldLinker::LoadMapSurfaces(const BSPData& bsp, GfxWorld& gfxWorld) const
    {
        LoadDrawData(bsp, gfxWorld);
        const auto plan = ReadLightmapPlan(m_search_path);

        size_t surfaceCount = bsp.gfxWorld.surfaces.size();
        std::vector<char> layerData;
        gfxWorld.surfaceCount = static_cast<int>(surfaceCount);
        gfxWorld.dpvs.staticSurfaceCount = static_cast<unsigned int>(surfaceCount);
        gfxWorld.dpvs.surfaces = m_memory.Alloc<GfxSurface>(surfaceCount);
        for (size_t surfIdx = 0; surfIdx < surfaceCount; surfIdx++)
        {
            const BSPSurface& bspSurface = bsp.gfxWorld.surfaces.at(surfIdx);
            GfxSurface* gfxSurface = &gfxWorld.dpvs.surfaces[surfIdx];

            gfxSurface->primaryLightIndex = bspSurface.primaryLightIndex < 0 ? BSPEditableConstants::DEFAULT_SURFACE_LIGHT
                : static_cast<unsigned char>(bspSurface.primaryLightIndex);
            gfxSurface->lightmapIndex = SurfaceLightmap(plan, bspSurface);
            gfxSurface->reflectionProbeIndex = BSPEditableConstants::DEFAULT_SURFACE_REFLECTION_PROBE;
            gfxSurface->flags = BSPEditableConstants::DEFAULT_SURFACE_FLAGS;

            gfxSurface->tris.triCount = static_cast<uint16_t>(bspSurface.triCount);
            gfxSurface->tris.baseIndex = static_cast<int>(bspSurface.indexOfFirstIndex);

            gfxSurface->tris.vertexDataOffset0 = static_cast<int>(bspSurface.indexOfFirstVertex * sizeof(GfxPackedWorldVertex));
            gfxSurface->tris.vertexDataOffset1 = 0;

            std::string surfMaterialName;
            if (bspSurface.material.materialType == BSPMaterialType::MATERIAL_TYPE_TEXTURE)
                surfMaterialName = bspSurface.material.materialName;
            else // MATERIAL_TYPE_COLOUR || MATERIAL_TYPE_EMPTY
                surfMaterialName = BSPLinkingConstants::COLOR_ONLY_IMAGE_NAME;

            auto surfMaterialAsset = m_context.LoadDependency<AssetMaterial>(surfMaterialName);
            if (!surfMaterialAsset)
            {
                std::string missingImageName = BSPLinkingConstants::MISSING_IMAGE_NAME;
                surfMaterialAsset = m_context.LoadDependency<AssetMaterial>(missingImageName);
                if (!surfMaterialAsset)
                {
                    con::error("unable to load the missing image texture {}!", missingImageName);
                    return false;
                }
            }
            gfxSurface->material = surfMaterialAsset->Asset();

            GfxPackedWorldVertex* firstVert = reinterpret_cast<GfxPackedWorldVertex*>(&gfxWorld.draw.vd0.data[gfxSurface->tris.vertexDataOffset0]);
            gfxSurface->bounds[0].x = firstVert[0].xyz.x;
            gfxSurface->bounds[0].y = firstVert[0].xyz.y;
            gfxSurface->bounds[0].z = firstVert[0].xyz.z;
            gfxSurface->bounds[1].x = firstVert[0].xyz.x;
            gfxSurface->bounds[1].y = firstVert[0].xyz.y;
            gfxSurface->bounds[1].z = firstVert[0].xyz.z;
            uint16_t maxVertIndex = 0;
            for (size_t indexIdx = 0; indexIdx < static_cast<size_t>(gfxSurface->tris.triCount * 3); indexIdx++)
            {
                uint16_t vertIndex = gfxWorld.draw.indices[gfxSurface->tris.baseIndex + indexIdx];
                maxVertIndex = std::max(maxVertIndex, vertIndex);
                UpdateAABBWithPoint(firstVert[vertIndex].xyz, gfxSurface->bounds[0], gfxSurface->bounds[1]);
            }

            // Runtime culling and draw submission use these triangle bounds
            // and vertex range, not just the parallel dpvs surface bounds.
            gfxSurface->tris.mins = gfxSurface->bounds[0];
            gfxSurface->tris.maxs = gfxSurface->bounds[1];
            gfxSurface->tris.himipRadiusInvSq = 0.0f;
            gfxSurface->tris.vertexCount = static_cast<uint16_t>(maxVertIndex + 1);
            gfxSurface->tris.firstVertex = static_cast<int>(bspSurface.indexOfFirstVertex);
            AppendLayerVertices(bsp, bspSurface, *gfxSurface, gfxWorld, layerData);
        }
        if (!layerData.empty())
        {
            gfxWorld.draw.vertexDataSize1 = static_cast<unsigned int>(layerData.size());
            gfxWorld.draw.vd1.data = m_memory.Alloc<char>(layerData.size());
            std::memcpy(gfxWorld.draw.vd1.data, layerData.data(), layerData.size());
        }

        // doesn't seem to matter what order the sorted surfs go in
        gfxWorld.dpvs.sortedSurfIndex = m_memory.Alloc<uint16_t>(surfaceCount);
        for (size_t surfIdx = 0; surfIdx < surfaceCount; surfIdx++)
            gfxWorld.dpvs.sortedSurfIndex[surfIdx] = static_cast<uint16_t>(surfIdx);

        // surface materials are written to by the game
        gfxWorld.dpvs.surfaceMaterials = m_memory.Alloc<GfxDrawSurf_align4>(surfaceCount);

        // Surfaces are grouped by camera region as in compiled maps:
        // lit opaque | lit translucent | emissive opaque | emissive translucent,
        // each group ordered by material sort key. Drawing decals and glass in
        // the opaque pass is wrong.
        const auto regionGroup = [](const GfxSurface& surf)
        {
            switch (surf.material ? surf.material->cameraRegion : CAMERA_REGION_LIT_OPAQUE)
            {
            case CAMERA_REGION_LIT_TRANS:
                return 1;
            case CAMERA_REGION_EMISSIVE_OPAQUE:
                return 2;
            case CAMERA_REGION_EMISSIVE_TRANS:
            case CAMERA_REGION_EMISSIVE_FX:
                return 3;
            default:
                return 0;
            }
        };
        // GfxSurface is over-aligned, so sort an index permutation and copy
        std::vector<size_t> order(surfaceCount);
        for (size_t i = 0; i < surfaceCount; i++)
            order[i] = i;
        const auto* unsorted = gfxWorld.dpvs.surfaces;
        std::ranges::stable_sort(order,
                                 [&](const size_t ia, const size_t ib)
                                 {
                                     const auto ownerA = bsp.gfxWorld.surfaces[ia].brushModel;
                                     const auto ownerB = bsp.gfxWorld.surfaces[ib].brushModel;
                                     if (ownerA != ownerB)
                                         return ownerA < ownerB;
                                     const auto& a = unsorted[ia];
                                     const auto& b = unsorted[ib];
                                     const auto ga = regionGroup(a), gb = regionGroup(b);
                                     if (ga != gb)
                                         return ga < gb;
                                     return (a.material ? a.material->info.sortKey : 0) < (b.material ? b.material->info.sortKey : 0);
                                 });
        auto* sorted = m_memory.Alloc<GfxSurface>(surfaceCount);
        for (size_t i = 0; i < surfaceCount; i++)
            sorted[i] = unsorted[order[i]];
        gfxWorld.dpvs.surfaces = sorted;
        m_surface_order = order;
        m_brush_surface_ranges.clear();
        m_brush_surface_ranges.resize(1);
        for (size_t i = 0; i < surfaceCount; i++)
        {
            const auto owner = bsp.gfxWorld.surfaces[order[i]].brushModel;
            if (owner >= m_brush_surface_ranges.size())
                m_brush_surface_ranges.resize(owner + 1);
            auto& range = m_brush_surface_ranges[owner];
            if (!range.second)
                range.first = static_cast<unsigned>(i);
            range.second++;
        }
        gfxWorld.dpvs.staticSurfaceCount = m_brush_surface_ranges[0].second;
        unsigned int groupEnd[4] = {};
        for (size_t i = 0; i < gfxWorld.dpvs.staticSurfaceCount; i++)
            groupEnd[regionGroup(sorted[i])]++;
        for (auto g = 1; g < 4; g++)
            groupEnd[g] += groupEnd[g - 1];
        gfxWorld.dpvs.litSurfsBegin = 0;
        gfxWorld.dpvs.litSurfsEnd = groupEnd[0];
        gfxWorld.dpvs.litTransSurfsBegin = groupEnd[0];
        gfxWorld.dpvs.litTransSurfsEnd = groupEnd[1];
        gfxWorld.dpvs.emissiveOpaqueSurfsBegin = groupEnd[1];
        gfxWorld.dpvs.emissiveOpaqueSurfsEnd = groupEnd[2];
        gfxWorld.dpvs.emissiveTransSurfsBegin = groupEnd[2];
        gfxWorld.dpvs.emissiveTransSurfsEnd = groupEnd[3];
        con::info("World surfaces: {} lit, {} lit translucent, {} emissive opaque, {} emissive translucent", groupEnd[0], groupEnd[1] - groupEnd[0],
                  groupEnd[2] - groupEnd[1], groupEnd[3] - groupEnd[2]);

        // visdata is written to by the game
        // all visdata is alligned by 128
        auto alignedSurfaceCount = utils::Align(surfaceCount, 128uz);
        gfxWorld.dpvs.surfaceVisDataCount = static_cast<unsigned int>(alignedSurfaceCount);
        gfxWorld.dpvs.surfaceVisData[0] = m_memory.Alloc<char>(alignedSurfaceCount);
        gfxWorld.dpvs.surfaceVisData[1] = m_memory.Alloc<char>(alignedSurfaceCount);
        gfxWorld.dpvs.surfaceVisData[2] = m_memory.Alloc<char>(alignedSurfaceCount);
        gfxWorld.dpvs.surfaceVisDataCameraSaved = m_memory.Alloc<char>(alignedSurfaceCount);
        gfxWorld.dpvs.surfaceCastsShadow = m_memory.Alloc<char>(alignedSurfaceCount);
        gfxWorld.dpvs.surfaceCastsSunShadow = m_memory.Alloc<char>(alignedSurfaceCount);

        return true;
    }

    bool GfxWorldLinker::LoadXModels(const BSPData& bsp, GfxWorld& gfxWorld) const
    {
        // Static model placements come from BSP/models.json (game coordinates):
        // { "models": [ { "name", "origin": [3], "axis": [9], "scale", "flags" } ] }
        // The file is optional; without it the map has no static models.
        std::vector<GfxStaticModelDrawInst> drawInsts;
        std::vector<GfxStaticModelInst> insts;

        const auto modelFilePath = GetFileNameForBSPAsset("models.json");
        const auto modelFile = m_search_path.Open(modelFilePath);
        if (modelFile.IsOpen())
        {
            json modelsJs;
            try
            {
                modelsJs = json::parse(*modelFile.m_stream);
            }
            catch (const json::exception& e)
            {
                con::error("JSON error when parsing {}: {}", modelFilePath, e.what());
                return false;
            }

            for (const auto& entry : modelsJs.at("models"))
            {
                const auto name = entry.at("name").get<std::string>();
                auto* xModelAsset = m_context.LoadDependency<AssetXModel>(name);
                if (!xModelAsset)
                {
                    con::error("ERROR! static model xmodel {} could not be loaded!", name);
                    return false;
                }
                const auto* model = xModelAsset->Asset();

                GfxStaticModelDrawInst drawInst{};
                const auto& origin = entry.at("origin");
                const auto& axis = entry.at("axis");
                drawInst.placement.origin.x = origin.at(0).get<float>();
                drawInst.placement.origin.y = origin.at(1).get<float>();
                drawInst.placement.origin.z = origin.at(2).get<float>();
                for (auto row = 0u; row < 3u; row++)
                {
                    drawInst.placement.axis[row].x = axis.at(row * 3 + 0).get<float>();
                    drawInst.placement.axis[row].y = axis.at(row * 3 + 1).get<float>();
                    drawInst.placement.axis[row].z = axis.at(row * 3 + 2).get<float>();
                }
                drawInst.placement.scale = entry.at("scale").get<float>();
                drawInst.model = const_cast<XModel*>(model);
                // the source map's per-instance draw distance; the default only when absent
                const auto cullDist = entry.value("cullDist", 0.0f);
                drawInst.cullDist = cullDist > 0.0f ? cullDist : BSPEditableConstants::DEFAULT_SMODEL_CULL_DIST;
                drawInst.flags = BSPEditableConstants::DEFAULT_SMODEL_FLAGS;
                drawInst.primaryLightIndex = entry.value("primaryLightIndex", BSPEditableConstants::DEFAULT_SMODEL_LIGHT);
                drawInst.reflectionProbeIndex = BSPEditableConstants::DEFAULT_SMODEL_REFLECTION_PROBE;
                drawInst.smid = static_cast<unsigned int>(drawInsts.size());

                // world-space bounds of the placed model: transform all 8 corners
                GfxStaticModelInst inst{};
                const auto& p = drawInst.placement;
                for (auto corner = 0u; corner < 8u; corner++)
                {
                    const auto lx = (corner & 1) ? model->maxs.x : model->mins.x;
                    const auto ly = (corner & 2) ? model->maxs.y : model->mins.y;
                    const auto lz = (corner & 4) ? model->maxs.z : model->mins.z;
                    vec3_t world;
                    world.x = p.origin.x + p.scale * (lx * p.axis[0].x + ly * p.axis[1].x + lz * p.axis[2].x);
                    world.y = p.origin.y + p.scale * (lx * p.axis[0].y + ly * p.axis[1].y + lz * p.axis[2].y);
                    world.z = p.origin.z + p.scale * (lx * p.axis[0].z + ly * p.axis[1].z + lz * p.axis[2].z);
                    if (corner == 0)
                    {
                        inst.mins = world;
                        inst.maxs = world;
                    }
                    UpdateAABBWithPoint(world, inst.mins, inst.maxs);
                }
                inst.lightingOrigin = CalcMiddleOfAABB(inst.mins, inst.maxs);

                drawInsts.emplace_back(drawInst);
                insts.emplace_back(inst);
            }
        }
        else
        {
            con::warn("Can't find static model file {}, the map will have no static models", modelFilePath);
        }

        const auto modelCount = static_cast<unsigned int>(drawInsts.size());
        if (modelCount > std::numeric_limits<uint16_t>::max())
        {
            con::error("ERROR! {} static models exceed the limit of {}", modelCount, std::numeric_limits<uint16_t>::max());
            return false;
        }
        gfxWorld.dpvs.smodelCount = modelCount;
        gfxWorld.dpvs.smodelInsts = m_memory.Alloc<GfxStaticModelInst>(modelCount);
        gfxWorld.dpvs.smodelDrawInsts = m_memory.Alloc<GfxStaticModelDrawInst>(modelCount);
        if (modelCount)
        {
            memcpy(gfxWorld.dpvs.smodelInsts, insts.data(), sizeof(GfxStaticModelInst) * modelCount);
            memcpy(gfxWorld.dpvs.smodelDrawInsts, drawInsts.data(), sizeof(GfxStaticModelDrawInst) * modelCount);
        }
        con::info("Loaded {} static models", modelCount);

        // visdata is written to by the game
        // all visdata is aligned by 128
        const auto alignedModelCount = utils::Align(modelCount, 128u);
        gfxWorld.dpvs.smodelVisDataCount = static_cast<unsigned int>(alignedModelCount);
        gfxWorld.dpvs.smodelVisData[0] = m_memory.Alloc<char>(alignedModelCount);
        gfxWorld.dpvs.smodelVisData[1] = m_memory.Alloc<char>(alignedModelCount);
        gfxWorld.dpvs.smodelVisData[2] = m_memory.Alloc<char>(alignedModelCount);
        gfxWorld.dpvs.smodelVisDataCameraSaved = m_memory.Alloc<char>(alignedModelCount);
        gfxWorld.dpvs.smodelCastsShadow = m_memory.Alloc<char>(alignedModelCount);
        for (unsigned int i = 0; i < modelCount; i++)
        {
            if ((gfxWorld.dpvs.smodelDrawInsts[i].flags & STATIC_MODEL_FLAG_NO_SHADOW) == 0)
                gfxWorld.dpvs.smodelCastsShadow[i] = 1;
            else
                gfxWorld.dpvs.smodelCastsShadow[i] = 0;
        }

        // official maps set this to 0
        gfxWorld.dpvs.usageCount = 0;

        return true;
    }

    void GfxWorldLinker::CleanGfxWorld(GfxWorld& gfxWorld) const
    {
        // checksum is generated by the game
        gfxWorld.checksum = 0;

        // Remove Coronas
        gfxWorld.coronaCount = 0;
        gfxWorld.coronas = nullptr;

        // Remove exposure volumes
        gfxWorld.exposureVolumeCount = 0;
        gfxWorld.exposureVolumes = nullptr;
        gfxWorld.exposureVolumePlaneCount = 0;
        gfxWorld.exposureVolumePlanes = nullptr;

        // Remove hero lights
        gfxWorld.heroLightCount = 0;
        gfxWorld.heroLights = nullptr;
        gfxWorld.heroLightTreeCount = 0;
        gfxWorld.heroLightTree = nullptr;

        // remove LUT data
        gfxWorld.lutVolumeCount = 0;
        gfxWorld.lutVolumes = nullptr;
        gfxWorld.lutVolumePlaneCount = 0;
        gfxWorld.lutVolumePlanes = nullptr;

        // remove occluders
        gfxWorld.numOccluders = 0;
        gfxWorld.occluders = nullptr;

        // remove Siege Skins
        gfxWorld.numSiegeSkinInsts = 0;
        gfxWorld.siegeSkinInsts = nullptr;

        // remove outdoor bounds
        gfxWorld.numOutdoorBounds = 0;
        gfxWorld.outdoorBounds = nullptr;

        // remove materials
        gfxWorld.ropeMaterial = nullptr;
        gfxWorld.lutMaterial = nullptr;
        gfxWorld.waterMaterial = nullptr;
        gfxWorld.coronaMaterial = nullptr;

        // remove shadow maps
        gfxWorld.shadowMapVolumeCount = 0;
        gfxWorld.shadowMapVolumes = nullptr;
        gfxWorld.shadowMapVolumePlaneCount = 0;
        gfxWorld.shadowMapVolumePlanes = nullptr;

        // remove stream info
        gfxWorld.streamInfo.aabbTreeCount = 0;
        gfxWorld.streamInfo.aabbTrees = nullptr;
        gfxWorld.streamInfo.leafRefCount = 0;
        gfxWorld.streamInfo.leafRefs = nullptr;

        // remove sun data
        memset(&gfxWorld.sun, 0, sizeof(sunflare_t));
        gfxWorld.sun.hasValidData = false;

        // Remove Water
        gfxWorld.waterDirection = 0.0f;
        gfxWorld.waterBuffers[0].bufferSize = 0;
        gfxWorld.waterBuffers[0].buffer = nullptr;
        gfxWorld.waterBuffers[1].bufferSize = 0;
        gfxWorld.waterBuffers[1].buffer = nullptr;

        // Remove Fog
        gfxWorld.worldFogModifierVolumeCount = 0;
        gfxWorld.worldFogModifierVolumes = nullptr;
        gfxWorld.worldFogModifierVolumePlaneCount = 0;
        gfxWorld.worldFogModifierVolumePlanes = nullptr;
        gfxWorld.worldFogVolumeCount = 0;
        gfxWorld.worldFogVolumes = nullptr;
        gfxWorld.worldFogVolumePlaneCount = 0;
        gfxWorld.worldFogVolumePlanes = nullptr;

        // materialMemory is unused
        gfxWorld.materialMemoryCount = 0;
        gfxWorld.materialMemory = nullptr;

        // sunLight is overwritten by the game, just needs to be a valid pointer
        gfxWorld.sunLight = m_memory.Alloc<GfxLight>();
    }

    void GfxWorldLinker::LoadGfxLights(GfxWorld& gfxWorld) const
    {
        // there must be 2 or more lights, first is the static light and second is the sun light
        gfxWorld.primaryLightCount = BSPGameConstants::BSP_DEFAULT_LIGHT_COUNT;
        if (const auto* world = m_context.LoadDependency<AssetComWorld>(gfxWorld.name))
            gfxWorld.primaryLightCount = world->Asset()->primaryLightCount;
        gfxWorld.sunPrimaryLightIndex = BSPGameConstants::SUN_LIGHT_INDEX;

        gfxWorld.shadowGeom = m_memory.Alloc<GfxShadowGeometry>(gfxWorld.primaryLightCount);
        for (unsigned int lightIdx = 0; lightIdx < gfxWorld.primaryLightCount; lightIdx++)
        {
            gfxWorld.shadowGeom[lightIdx].smodelCount = 0;
            gfxWorld.shadowGeom[lightIdx].surfaceCount = 0;
            gfxWorld.shadowGeom[lightIdx].smodelIndex = nullptr;
            gfxWorld.shadowGeom[lightIdx].sortedSurfIndex = nullptr;
        }

        gfxWorld.lightRegion = m_memory.Alloc<GfxLightRegion>(gfxWorld.primaryLightCount);
        for (unsigned int lightIdx = 0; lightIdx < gfxWorld.primaryLightCount; lightIdx++)
        {
            gfxWorld.lightRegion[lightIdx].hullCount = 0;
            gfxWorld.lightRegion[lightIdx].hulls = nullptr;
        }

        unsigned int lightEntShadowVisSize = (gfxWorld.primaryLightCount - gfxWorld.sunPrimaryLightIndex - 1) * 8192;
        if (lightEntShadowVisSize != 0)
            gfxWorld.primaryLightEntityShadowVis = m_memory.Alloc<unsigned int>(lightEntShadowVisSize);
        else
            gfxWorld.primaryLightEntityShadowVis = nullptr;
    }

    void GfxWorldLinker::LoadShadowGeometry(const BSPData& bsp, GfxWorld& gfxWorld) const
    {
        // BSP/shadowgeom.json (waw2bo2): per primary light, the static world
        // meshes and static models drawn into its shadow map and its light
        // region hulls: the WaW GfxShadowGeometry / GfxLightRegion (same layout in T6).
        const auto file = m_search_path.Open(GetFileNameForBSPAsset("shadowgeom.json"));
        if (!file.IsOpen())
            return;
        json js;
        try
        {
            js = json::parse(*file.m_stream);
        }
        catch (const json::exception& e)
        {
            con::error("JSON error when parsing shadowgeom.json: {}", e.what());
            return;
        }
        const auto& lights = js.at("lights");
        if (lights.size() != gfxWorld.primaryLightCount)
        {
            con::error("shadowgeom.json has {} lights, the world {}; shadow geometry not loaded", lights.size(),
                       gfxWorld.primaryLightCount);
            return;
        }
        // static world surfaces only (brush model surfaces move)
        std::map<int, std::vector<uint16_t>> surfacesOfMesh;
        for (size_t i = 0; i < m_surface_order.size() && i < gfxWorld.dpvs.staticSurfaceCount; ++i)
            surfacesOfMesh[bsp.gfxWorld.surfaces[m_surface_order[i]].meshIndex].push_back(static_cast<uint16_t>(i));
        for (unsigned light = 0; light < gfxWorld.primaryLightCount; ++light)
        {
            const auto& entry = lights[light];
            std::vector<uint16_t> surfaces;
            for (const auto& mesh : entry.at("meshes"))
            {
                const auto found = surfacesOfMesh.find(mesh.get<int>());
                if (found != surfacesOfMesh.end())
                    surfaces.insert(surfaces.end(), found->second.begin(), found->second.end());
            }
            std::ranges::sort(surfaces);
            surfaces.erase(std::ranges::unique(surfaces).begin(), surfaces.end());
            std::vector<uint16_t> smodels;
            for (const auto& smodel : entry.at("smodels"))
                if (smodel.get<unsigned>() < gfxWorld.dpvs.smodelCount)
                    smodels.push_back(smodel.get<uint16_t>());
            auto& geom = gfxWorld.shadowGeom[light];
            geom.surfaceCount = static_cast<uint16_t>(surfaces.size());
            geom.smodelCount = static_cast<uint16_t>(smodels.size());
            geom.sortedSurfIndex = surfaces.empty() ? nullptr : m_memory.Alloc<uint16_t>(surfaces.size());
            geom.smodelIndex = smodels.empty() ? nullptr : m_memory.Alloc<uint16_t>(smodels.size());
            std::ranges::copy(surfaces, geom.sortedSurfIndex);
            std::ranges::copy(smodels, geom.smodelIndex);

            const auto& hulls = entry.at("hulls");
            auto& region = gfxWorld.lightRegion[light];
            region.hullCount = static_cast<unsigned>(hulls.size());
            region.hulls = hulls.empty() ? nullptr : m_memory.Alloc<GfxLightRegionHull>(hulls.size());
            for (size_t h = 0; h < hulls.size(); ++h)
            {
                auto& hull = region.hulls[h];
                for (unsigned k = 0; k < 9; ++k)
                {
                    hull.kdopMidPoint[k] = hulls[h].at("kdopMidPoint").at(k).get<float>();
                    hull.kdopHalfSize[k] = hulls[h].at("kdopHalfSize").at(k).get<float>();
                }
                const auto& axes = hulls[h].at("axes");
                hull.axisCount = static_cast<unsigned>(axes.size());
                hull.axis = axes.empty() ? nullptr : m_memory.Alloc<GfxLightRegionAxis>(axes.size());
                for (size_t a = 0; a < axes.size(); ++a)
                {
                    for (unsigned k = 0; k < 3; ++k)
                        hull.axis[a].dir.v[k] = axes[a].at("dir").at(k).get<float>();
                    hull.axis[a].midPoint = axes[a].at("midPoint").get<float>();
                    hull.axis[a].halfSize = axes[a].at("halfSize").get<float>();
                }
            }
        }
        // WaW draws the static models listed in the sun's shadow geometry into
        // the sun shadow map; the others cast no sun shadow.
        const auto& sun = gfxWorld.shadowGeom[gfxWorld.sunPrimaryLightIndex];
        std::vector<char> sunCaster(gfxWorld.dpvs.smodelCount, 0);
        for (unsigned i = 0; i < sun.smodelCount; ++i)
            sunCaster[sun.smodelIndex[i]] = 1;
        for (unsigned i = 0; i < gfxWorld.dpvs.smodelCount; ++i)
        {
            auto& flags = gfxWorld.dpvs.smodelDrawInsts[i].flags;
            flags = sunCaster[i] ? (flags & ~STATIC_MODEL_FLAG_NO_SHADOW) : (flags | STATIC_MODEL_FLAG_NO_SHADOW);
            gfxWorld.dpvs.smodelCastsShadow[i] = sunCaster[i];
        }
        con::info("Loaded shadow geometry for {} primary lights ({} sun-shadowing static models)", lights.size(),
                  sun.smodelCount);
    }

    bool GfxWorldLinker::LoadLightGrid(GfxWorld& gfxWorld) const
    {
        const auto file = m_search_path.Open(GetFileNameForBSPAsset("lightgrid.bin"));
        if (file.IsOpen())
        {
            auto& stream = *file.m_stream;
            auto& grid = gfxWorld.lightGrid;
            const auto read = [&stream](auto& value) { stream.read(reinterpret_cast<char*>(&value), sizeof(value)); };
            char magic[8]; uint32_t version, regions, rowCount;
            read(magic); read(version); read(regions); read(grid.sunPrimaryLightIndex);
            read(grid.mins); read(grid.maxs); read(grid.rowAxis); read(grid.colAxis); read(rowCount);
            read(grid.rawRowDataSize); read(grid.entryCount); read(grid.colorCount);
            if (!stream || std::memcmp(magic, "W2BT6LG1", 8) || version != 1 || grid.rowAxis > 1 || grid.colAxis > 1
                || grid.rowAxis == grid.colAxis || grid.maxs[grid.rowAxis] < grid.mins[grid.rowAxis]
                || rowCount != grid.maxs[grid.rowAxis] - grid.mins[grid.rowAxis] + 1u
                || grid.sunPrimaryLightIndex >= gfxWorld.primaryLightCount)
            {
                con::error("Invalid translated light-grid header"); return false;
            }
            const uint64_t expected = 56ull + rowCount * 2ull + grid.rawRowDataSize + grid.entryCount * 4ull + grid.colorCount * 168ull;
            if (expected != static_cast<uint64_t>(file.m_length))
            {
                con::error("Invalid translated light-grid payload length"); return false;
            }
            grid.rowDataStart = m_memory.Alloc<uint16_t>(rowCount);
            grid.rawRowData = static_cast<aligned_byte_pointer*>(m_memory.AllocRaw(grid.rawRowDataSize));
            grid.entries = m_memory.Alloc<GfxLightGridEntry>(grid.entryCount);
            grid.colors = m_memory.Alloc<GfxCompressedLightGridColors>(grid.colorCount);
            stream.read(reinterpret_cast<char*>(grid.rowDataStart), rowCount * 2u);
            stream.read(reinterpret_cast<char*>(grid.rawRowData), grid.rawRowDataSize);
            stream.read(reinterpret_cast<char*>(grid.entries), grid.entryCount * 4u);
            stream.read(reinterpret_cast<char*>(grid.colors), grid.colorCount * 168u);
            for (unsigned i = 0; i < grid.entryCount; ++i)
                if (grid.entries[i].colorsIndex >= grid.colorCount
                    || (static_cast<unsigned char>(grid.entries[i].primaryLightIndex) != 255
                        && static_cast<unsigned char>(grid.entries[i].primaryLightIndex) >= gfxWorld.primaryLightCount))
                {
                    con::error("Light-grid entry {} references an absent palette or primary light", i); return false;
                }
            grid.offset = 0.0f;
            grid.coeffCount = 0; grid.coeffs = nullptr;
            grid.skyGridVolumeCount = 0; grid.skyGridVolumes = nullptr;
            con::info("Loaded source light grid: {} rows, {} entries, {} palettes", rowCount, grid.entryCount, grid.colorCount);
            return static_cast<bool>(stream);
        }
        // there is almost no basis for the values in this code, they were chosen based on what looks correct when reverse engineering.

        // mins and maxs define the range that the lightgrid will work in.
        // unknown how these values are calculated, but the below values are larger
        // than official map values
        gfxWorld.lightGrid.mins[0] = 0;
        gfxWorld.lightGrid.mins[1] = 0;
        gfxWorld.lightGrid.mins[2] = 0;
        gfxWorld.lightGrid.maxs[0] = 200;
        gfxWorld.lightGrid.maxs[1] = 200;
        gfxWorld.lightGrid.maxs[2] = 50;

        gfxWorld.lightGrid.rowAxis = 0; // default value
        gfxWorld.lightGrid.colAxis = 1; // default value
        gfxWorld.lightGrid.sunPrimaryLightIndex = BSPGameConstants::SUN_LIGHT_INDEX;
        gfxWorld.lightGrid.offset = 0.0f; // default value

        // setting all rowDataStart indexes to 0 will always index the first row in rawRowData
        int rowDataStartSize = gfxWorld.lightGrid.maxs[gfxWorld.lightGrid.rowAxis] - gfxWorld.lightGrid.mins[gfxWorld.lightGrid.rowAxis] + 1;
        gfxWorld.lightGrid.rowDataStart = m_memory.Alloc<uint16_t>(rowDataStartSize);

        // Adding 0x0F so the lookup table will be 0x10 bytes in size
        gfxWorld.lightGrid.rawRowDataSize = static_cast<unsigned int>(sizeof(GfxLightGridRow) + 0x0F);
        GfxLightGridRow* row = static_cast<GfxLightGridRow*>(m_memory.AllocRaw(gfxWorld.lightGrid.rawRowDataSize));
        row->colStart = 0;
        row->colCount = 0x1000; // 0x1000 as this is large enough for all checks done by the game
        row->zStart = 0;
        row->zCount = 0xFF; // 0xFF as this is large enough for all checks done by the game, but small enough not to mess with other checks
        row->firstEntry = 0;
        for (int i = 0; i < 0x10; i++) // set the lookup table to all 0
            row->lookupTable[i] = 0;
        gfxWorld.lightGrid.rawRowData = reinterpret_cast<aligned_byte_pointer*>(row);

        // entries are looked up based on the lightgrid sample pos (given ingame) and the lightgrid lookup table
        gfxWorld.lightGrid.entryCount = 60000; // 60000 as it should be enough entries to be indexed by all lightgrid sample positions
        GfxLightGridEntry* entryArray = m_memory.Alloc<GfxLightGridEntry>(gfxWorld.lightGrid.entryCount);
        for (unsigned int i = 0; i < gfxWorld.lightGrid.entryCount; i++)
        {
            entryArray[i].colorsIndex = 0; // always index first colour
            entryArray[i].primaryLightIndex = BSPGameConstants::SUN_LIGHT_INDEX;
            entryArray[i].visibility = 0;
        }
        gfxWorld.lightGrid.entries = entryArray;

        // colours are looked up with a lightgrid entries colorsIndex
        gfxWorld.lightGrid.colorCount = 0x1000; // 0x1000 as it should be enough to hold every index
        gfxWorld.lightGrid.colors = m_memory.Alloc<GfxCompressedLightGridColors>(gfxWorld.lightGrid.colorCount);
        memset(gfxWorld.lightGrid.colors, BSPEditableConstants::LIGHTGRID_COLOUR, rowDataStartSize * sizeof(uint16_t));

        // we use the colours array instead of coeffs array
        gfxWorld.lightGrid.coeffCount = 0;
        gfxWorld.lightGrid.coeffs = nullptr;
        gfxWorld.lightGrid.skyGridVolumeCount = 0;
        gfxWorld.lightGrid.skyGridVolumes = nullptr;
        return true;
    }

    void GfxWorldLinker::LoadGfxCells(GfxWorld& gfxWorld) const
    {
        // Cells are basically data used to determine what can be seen and what cant be seen
        // Right now custom maps have no optimisation so there is only 1 cell
        int cellCount = 1;

        gfxWorld.dpvsPlanes.cellCount = cellCount;
        gfxWorld.cellBitsCount = ((cellCount + 127) >> 3) & 0x1FFFFFF0;

        int cellCasterBitsCount = cellCount * ((cellCount + 31) / 32);
        gfxWorld.cellCasterBits = m_memory.Alloc<unsigned int>(cellCasterBitsCount);

        int sceneEntCellBitsCount = cellCount * 512;
        gfxWorld.dpvsPlanes.sceneEntCellBits = m_memory.Alloc<unsigned int>(sceneEntCellBitsCount);

        gfxWorld.cells = m_memory.Alloc<GfxCell>(cellCount);
        gfxWorld.cells[0].portalCount = 0;
        gfxWorld.cells[0].portals = nullptr;
        gfxWorld.cells[0].mins.x = gfxWorld.mins.x;
        gfxWorld.cells[0].mins.y = gfxWorld.mins.y;
        gfxWorld.cells[0].mins.z = gfxWorld.mins.z;
        gfxWorld.cells[0].maxs.x = gfxWorld.maxs.x;
        gfxWorld.cells[0].maxs.y = gfxWorld.maxs.y;
        gfxWorld.cells[0].maxs.z = gfxWorld.maxs.z;

        // there is only 1 reflection probe
        gfxWorld.cells[0].reflectionProbeCount = 1;
        gfxWorld.cells[0].reflectionProbes = m_memory.Alloc<char>(gfxWorld.cells[0].reflectionProbeCount);
        gfxWorld.cells[0].reflectionProbes[0] = BSPEditableConstants::DEFAULT_SURFACE_REFLECTION_PROBE;

        // AABB trees decide what is drawn. A single node holding every surface
        // and static model queues the whole map each frame, which overflows
        // the renderer's per-frame lists on large maps (parts of the world
        // vanish, then the game crashes). Build a hierarchy like compiled maps.
        BuildCellAabbTree(m_memory, gfxWorld, gfxWorld.cells[0]);

        // nodes have the struct mnode_t, and there must be at least 1 node (similar to BSP nodes)
        // Nodes mnode_t.cellIndex indexes gfxWorld->cells
        // and (mnode_t.cellIndex - (world->dpvsPlanes.cellCount + 1) indexes world->dpvsPlanes.planes
        // Use only one node as there is no optimisation in custom maps
        gfxWorld.nodeCount = 1;
        gfxWorld.dpvsPlanes.nodes = m_memory.Alloc<uint16_t>(gfxWorld.nodeCount);
        gfxWorld.dpvsPlanes.nodes[0] = 1; // nodes reference cells by index + 1

        // planes are overwritten by the clipmap loading code ingame
        gfxWorld.planeCount = 0;
        gfxWorld.dpvsPlanes.planes = nullptr;
    }

    void GfxWorldLinker::LoadWorldBounds(GfxWorld& gfxWorld) const
    {
        gfxWorld.mins.x = 0.0f;
        gfxWorld.mins.y = 0.0f;
        gfxWorld.mins.z = 0.0f;
        gfxWorld.maxs.x = 0.0f;
        gfxWorld.maxs.y = 0.0f;
        gfxWorld.maxs.z = 0.0f;

        for (int surfIdx = 0; surfIdx < gfxWorld.surfaceCount; surfIdx++)
        {
            UpdateAABB(gfxWorld.dpvs.surfaces[surfIdx].bounds[0], gfxWorld.dpvs.surfaces[surfIdx].bounds[1], gfxWorld.mins, gfxWorld.maxs);
        }
        for (unsigned int smodelIdx = 0; smodelIdx < gfxWorld.dpvs.smodelCount; smodelIdx++)
        {
            UpdateAABB(gfxWorld.dpvs.smodelInsts[smodelIdx].mins, gfxWorld.dpvs.smodelInsts[smodelIdx].maxs, gfxWorld.mins, gfxWorld.maxs);
        }
    }

    void GfxWorldLinker::LoadModels(GfxWorld& gfxWorld) const
    {
        // Brush models keep collision bounds and their own contiguous render
        // ranges. Their surfaces follow the static-world camera region ranges.
        std::vector<std::pair<vec3_t, vec3_t>> subBounds;
        const auto subFile = m_search_path.Open(GetFileNameForBSPAsset("submodels.json"));
        if (subFile.IsOpen())
        {
            try
            {
                for (const auto& modelJs : json::parse(*subFile.m_stream).at("submodels"))
                {
                    const auto& mins = modelJs.at("mins");
                    const auto& maxs = modelJs.at("maxs");
                    subBounds.emplace_back(vec3_t{{.x = mins.at(0).get<float>(), .y = mins.at(1).get<float>(), .z = mins.at(2).get<float>()}},
                                           vec3_t{{.x = maxs.at(0).get<float>(), .y = maxs.at(1).get<float>(), .z = maxs.at(2).get<float>()}});
                }
            }
            catch (const json::exception& e)
            {
                con::error("JSON error when parsing submodels.json: {}", e.what());
                subBounds.clear();
            }
        }

        gfxWorld.modelCount = 1 + static_cast<int>(subBounds.size());
        gfxWorld.models = m_memory.Alloc<GfxBrushModel>(gfxWorld.modelCount);
        for (size_t subIdx = 0; subIdx < subBounds.size(); subIdx++)
        {
            auto& model = gfxWorld.models[subIdx + 1];
            const auto owner = subIdx + 1;
            model.startSurfIndex = owner < m_brush_surface_ranges.size() ? m_brush_surface_ranges[owner].first : 0;
            model.surfaceCount = owner < m_brush_surface_ranges.size() ? m_brush_surface_ranges[owner].second : 0;
            model.bounds[0] = subBounds[subIdx].first;
            model.bounds[1] = subBounds[subIdx].second;
            memset(&model.writable, 0, sizeof(GfxBrushModelWritable));
        }

        // first model is always the world model
        gfxWorld.models[0].startSurfIndex = 0;
        gfxWorld.models[0].surfaceCount = gfxWorld.dpvs.staticSurfaceCount;
        gfxWorld.models[0].bounds[0].x = gfxWorld.mins.x;
        gfxWorld.models[0].bounds[0].y = gfxWorld.mins.y;
        gfxWorld.models[0].bounds[0].z = gfxWorld.mins.z;
        gfxWorld.models[0].bounds[1].x = gfxWorld.maxs.x;
        gfxWorld.models[0].bounds[1].y = gfxWorld.maxs.y;
        gfxWorld.models[0].bounds[1].z = gfxWorld.maxs.z;
        memset(&gfxWorld.models[0].writable, 0, sizeof(GfxBrushModelWritable));

        // Other models aren't implemented yet
        // Code kept for future use
        // for (size_t i = 0; i < entityModelList.size(); i++)
        //{
        //    auto currEntModel = &gfxWorld->models[i + 1];
        //    entModelBounds currEntModelBounds = entityModelList[i];
        //
        //    currEntModel->startSurfIndex = 0;
        //    currEntModel->surfaceCount = -1; // -1 when it doesn't use map surfaces
        //    currEntModel->bounds[0].x = currEntModelBounds.mins.x;
        //    currEntModel->bounds[0].y = currEntModelBounds.mins.y;
        //    currEntModel->bounds[0].z = currEntModelBounds.mins.z;
        //    currEntModel->bounds[1].x = currEntModelBounds.maxs.x;
        //    currEntModel->bounds[1].y = currEntModelBounds.maxs.y;
        //    currEntModel->bounds[1].z = currEntModelBounds.maxs.z;
        //    memset(&gfxWorld->models[0].writable, 0, sizeof(GfxBrushModelWritable));
        //}
    }

    void GfxWorldLinker::LoadSunData(GfxWorld& gfxWorld) const
    {
        // default values taken from mp_dig
        gfxWorld.sunParse.fogTransitionTime = 0.001f;
        gfxWorld.sunParse.name[0] = 0x00;

        gfxWorld.sunParse.initWorldSun->control = 0;
        gfxWorld.sunParse.initWorldSun->exposure = 2.5f;
        gfxWorld.sunParse.initWorldSun->angles.x = -29.0f;
        gfxWorld.sunParse.initWorldSun->angles.y = 254.0f;
        gfxWorld.sunParse.initWorldSun->angles.z = 0.0f;
        gfxWorld.sunParse.initWorldSun->sunCd.x = 1.0f;
        gfxWorld.sunParse.initWorldSun->sunCd.y = 0.89f;
        gfxWorld.sunParse.initWorldSun->sunCd.z = 0.69f;
        gfxWorld.sunParse.initWorldSun->sunCd.w = 13.5f;
        gfxWorld.sunParse.initWorldSun->ambientColor.x = 0.0f;
        gfxWorld.sunParse.initWorldSun->ambientColor.y = 0.0f;
        gfxWorld.sunParse.initWorldSun->ambientColor.z = 0.0f;
        gfxWorld.sunParse.initWorldSun->ambientColor.w = 0.0f;
        gfxWorld.sunParse.initWorldSun->skyColor.x = 0.0f;
        gfxWorld.sunParse.initWorldSun->skyColor.y = 0.0f;
        gfxWorld.sunParse.initWorldSun->skyColor.z = 0.0f;
        gfxWorld.sunParse.initWorldSun->skyColor.w = 0.0f;
        gfxWorld.sunParse.initWorldSun->sunCs.x = 0.0f;
        gfxWorld.sunParse.initWorldSun->sunCs.y = 0.0f;
        gfxWorld.sunParse.initWorldSun->sunCs.z = 0.0f;
        gfxWorld.sunParse.initWorldSun->sunCs.w = 0.0f;

        gfxWorld.sunParse.initWorldFog->baseDist = 150.0f;
        gfxWorld.sunParse.initWorldFog->baseHeight = -100.0f;
        gfxWorld.sunParse.initWorldFog->fogColor.x = 2.35f;
        gfxWorld.sunParse.initWorldFog->fogColor.y = 3.10f;
        gfxWorld.sunParse.initWorldFog->fogColor.z = 3.84f;
        gfxWorld.sunParse.initWorldFog->fogOpacity = 0.52f;
        gfxWorld.sunParse.initWorldFog->halfDist = 4450.f;
        gfxWorld.sunParse.initWorldFog->halfHeight = 2000.f;
        gfxWorld.sunParse.initWorldFog->sunFogColor.x = 5.27f;
        gfxWorld.sunParse.initWorldFog->sunFogColor.y = 4.73f;
        gfxWorld.sunParse.initWorldFog->sunFogColor.z = 3.88f;
        gfxWorld.sunParse.initWorldFog->sunFogInner = 0.0f;
        gfxWorld.sunParse.initWorldFog->sunFogOpacity = 0.67f;
        gfxWorld.sunParse.initWorldFog->sunFogOuter = 80.84f;
        gfxWorld.sunParse.initWorldFog->sunFogPitch = -29.0f;
        gfxWorld.sunParse.initWorldFog->sunFogYaw = 254.0f;
        const auto file = m_search_path.Open(GetFileNameForBSPAsset("lighting.json"));
        if (file.IsOpen())
        {
            const auto data = json::parse(*file.m_stream);
            const auto& source = data.at("sun");
            auto& sun = *gfxWorld.sunParse.initWorldSun;
            for (unsigned i = 0; i < 3; ++i)
            {
                sun.angles.v[i] = source.at("angles").at(i).get<float>();
                // T6 stores the renderer's linear light, while WaW's source
                // parameters and translated programs operate in gamma units.
                const auto color = source.at("sunColor").at(i).get<float>();
                const auto ambient = source.at("ambientColor").at(i).get<float>();
                sun.sunCd.v[i] = color * color;
                sun.ambientColor.v[i] = ambient * ambient;
            }
            const auto ambientScale = source.at("ambientScale").get<float>();
            // T4 sub_705920 removes ambient and the diffuse share before
            // forming sunDiffuse. Squaring raw sunLight overlights T6 models.
            const auto strength = std::max(0.0f, (source.at("sunLight").get<float>() - ambientScale)
                * (1.0f - source.at("diffuseFraction").get<float>()));
            sun.sunCd.v[3] = strength * strength;
            sun.ambientColor.v[3] = ambientScale * ambientScale;
            // T6 sub_728180: hdrControl0.x = 1 / 2^(exposure + 2); lit programs
            // write sqrt(hdrControl0.x * linear) and every hdr_bloom_apply
            // composite displays sqrt(4 * buffer^2) (in game, every 2 stops
            // lower doubles the final pixel). WaW draws gamma colour directly;
            // the converted lightmaps, light grid, sun and fog hold WaW's
            // display values squared, shown 1:1 at hdrControl0.x = 1/4.
            constexpr auto unitScaleExposure = 0.0f;
            const auto fogScale = std::exp2(-(sun.exposure + 2.0f)) / std::exp2(-(unitScaleExposure + 2.0f));
            auto& fog = *gfxWorld.sunParse.initWorldFog;
            for (unsigned i = 0; i < 3; ++i)
            {
                // template fog keeps its on-screen colour until map script fog arrives
                fog.fogColor.v[i] *= fogScale;
                fog.sunFogColor.v[i] *= fogScale;
            }
            sun.exposure = data.value("exposure", unitScaleExposure);
            const auto name = source.value("name", std::string());
            std::strncpy(gfxWorld.sunParse.name, name.c_str(), sizeof(gfxWorld.sunParse.name) - 1);
            con::info("Loaded source sun angles and colors (WaW direct strength {}, exposure {})", strength, sun.exposure);
        }
    }

    bool GfxWorldLinker::LoadReflectionProbeData(GfxWorld& gfxWorld) const
    {
        gfxWorld.draw.reflectionProbeCount = 1;

        gfxWorld.draw.reflectionProbeTextures = m_memory.Alloc<GfxTexture>(gfxWorld.draw.reflectionProbeCount);

        // default values taken from mp_dig
        gfxWorld.draw.reflectionProbes = m_memory.Alloc<GfxReflectionProbe>(gfxWorld.draw.reflectionProbeCount);
        gfxWorld.draw.reflectionProbes[0].mipLodBias = -8.0;
        gfxWorld.draw.reflectionProbes[0].origin.x = 0.0f;
        gfxWorld.draw.reflectionProbes[0].origin.y = 0.0f;
        gfxWorld.draw.reflectionProbes[0].origin.z = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V0.x = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V0.y = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V0.z = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V0.w = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V1.x = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V1.y = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V1.z = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V1.w = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V2.x = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V2.y = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V2.z = 0.0f;
        gfxWorld.draw.reflectionProbes[0].lightingSH.V2.w = 0.0f;

        gfxWorld.draw.reflectionProbes[0].probeVolumeCount = 0;
        gfxWorld.draw.reflectionProbes[0].probeVolumes = nullptr;

        std::string probeImageName = "reflection_probe0";
        auto probeImageAsset = m_context.LoadDependency<AssetImage>(probeImageName);
        if (!probeImageAsset)
        {
            con::error("ERROR! unable to find reflection probe image {}!", probeImageName);
            return false;
        }
        gfxWorld.draw.reflectionProbes[0].reflectionImage = probeImageAsset->Asset();

        return true;
    }

    bool GfxWorldLinker::LoadLightmapData(GfxWorld& gfxWorld) const
    {
        const auto plan = ReadLightmapPlan(m_search_path);
        if (!plan.pages.empty())
        {
            // pages [0, n) hold the WaW-encoded copies, [n, 2n) the T6-encoded ones
            const auto pageCount = plan.pages.size();
            gfxWorld.draw.lightmapCount = static_cast<int>(pageCount * 2);
            gfxWorld.draw.lightmapPrimaryTextures = m_memory.Alloc<GfxTexture>(gfxWorld.draw.lightmapCount);
            gfxWorld.draw.lightmapSecondaryTextures = m_memory.Alloc<GfxTexture>(gfxWorld.draw.lightmapCount);
            gfxWorld.draw.lightmaps = m_memory.Alloc<GfxLightmapArray>(gfxWorld.draw.lightmapCount);
            for (size_t variant = 0; variant < 2; variant++)
            {
                for (size_t page = 0; page < pageCount; page++)
                {
                    const auto& name = variant == 0 ? plan.pages[page].first : plan.pages[page].second;
                    auto* image = m_context.LoadDependency<AssetImage>(name);
                    if (!image)
                    {
                        con::error("ERROR! unable to find lightmap image {}!", name);
                        return false;
                    }
                    auto& lightmap = gfxWorld.draw.lightmaps[variant * pageCount + page];
                    lightmap.primary = nullptr; // always nullptr in stock zones
                    lightmap.secondary = image->Asset();
                }
            }
            con::info("Linked {} lightmap pages ({} WaW-encoded, {} T6-encoded)", pageCount * 2, pageCount, pageCount);
            return true;
        }

        gfxWorld.draw.lightmapCount = 1;

        gfxWorld.draw.lightmapPrimaryTextures = m_memory.Alloc<GfxTexture>(gfxWorld.draw.lightmapCount);
        gfxWorld.draw.lightmapSecondaryTextures = m_memory.Alloc<GfxTexture>(gfxWorld.draw.lightmapCount);

        std::string secondaryTexture = "lightmap0_secondary";
        auto secondaryTextureAsset = m_context.LoadDependency<AssetImage>(secondaryTexture);
        if (!secondaryTextureAsset)
        {
            con::error("ERROR! unable to find lightmap image {}!", secondaryTexture);
            return false;
        }
        gfxWorld.draw.lightmaps = m_memory.Alloc<GfxLightmapArray>(gfxWorld.draw.lightmapCount);
        gfxWorld.draw.lightmaps[0].primary = nullptr; // always nullptr
        gfxWorld.draw.lightmaps[0].secondary = secondaryTextureAsset->Asset();

        return true;
    }

    void GfxWorldLinker::LoadSkyBox(const BSPData& projInfo, GfxWorld& gfxWorld) const
    {
        const auto skyBoxName = std::format("skybox_{}", projInfo.name);
        gfxWorld.skyBoxModel = m_memory.Dup(skyBoxName.c_str());

        if (!m_context.LoadDependency<AssetXModel>(skyBoxName))
        {
            con::warn("WARN: Unable to load the skybox xmodel {}!", skyBoxName);
        }

        // default skybox values from mp_dig
        gfxWorld.skyDynIntensity.angle0 = 0.0f;
        gfxWorld.skyDynIntensity.angle1 = 0.0f;
        gfxWorld.skyDynIntensity.factor0 = 1.0f;
        gfxWorld.skyDynIntensity.factor1 = 1.0f;
    }

    void GfxWorldLinker::LoadDynEntData(GfxWorld& gfxWorld) const
    {
        unsigned dynEntCount = 0u;
        gfxWorld.dpvsDyn.dynEntClientCount[0] = dynEntCount + 256; // the game allocs 256 empty dynents, as they may be used ingame
        gfxWorld.dpvsDyn.dynEntClientCount[1] = 0;

        // +100: there is a crash that happens when regdolls are created, and dynEntClientWordCount[0] is the issue.
        // Making the value much larger than required fixes it, but unsure what the root cause is
        gfxWorld.dpvsDyn.dynEntClientWordCount[0] = ((gfxWorld.dpvsDyn.dynEntClientCount[0] + 31) >> 5) + 100;
        gfxWorld.dpvsDyn.dynEntClientWordCount[1] = 0;
        gfxWorld.dpvsDyn.usageCount = 0;

        const auto dynEntCellBitsSize = gfxWorld.dpvsDyn.dynEntClientWordCount[0] * static_cast<unsigned>(gfxWorld.dpvsPlanes.cellCount);
        gfxWorld.dpvsDyn.dynEntCellBits[0] = m_memory.Alloc<unsigned int>(dynEntCellBitsSize);
        gfxWorld.dpvsDyn.dynEntCellBits[1] = nullptr;

        const auto dynEntVisData0Size = gfxWorld.dpvsDyn.dynEntClientWordCount[0] * 32u;
        gfxWorld.dpvsDyn.dynEntVisData[0][0] = m_memory.Alloc<char>(dynEntVisData0Size);
        gfxWorld.dpvsDyn.dynEntVisData[0][1] = m_memory.Alloc<char>(dynEntVisData0Size);
        gfxWorld.dpvsDyn.dynEntVisData[0][2] = m_memory.Alloc<char>(dynEntVisData0Size);
        gfxWorld.dpvsDyn.dynEntVisData[1][0] = nullptr;
        gfxWorld.dpvsDyn.dynEntVisData[1][1] = nullptr;
        gfxWorld.dpvsDyn.dynEntVisData[1][2] = nullptr;

        const auto dynEntShadowVisCount = gfxWorld.dpvsDyn.dynEntClientCount[0] * (gfxWorld.primaryLightCount - gfxWorld.sunPrimaryLightIndex - 1u);
        gfxWorld.primaryLightDynEntShadowVis[0] = m_memory.Alloc<unsigned int>(dynEntShadowVisCount);
        gfxWorld.primaryLightDynEntShadowVis[1] = nullptr;

        gfxWorld.sceneDynModel = m_memory.Alloc<GfxSceneDynModel>(gfxWorld.dpvsDyn.dynEntClientCount[0]);
        gfxWorld.sceneDynBrush = nullptr;
    }

    bool GfxWorldLinker::LoadOutdoors(GfxWorld& gfxWorld) const
    {
        const auto xRecip = 1.0f / (gfxWorld.maxs.x - gfxWorld.mins.x);
        const auto xScale = -(xRecip * gfxWorld.mins.x);

        const auto yRecip = 1.0f / (gfxWorld.maxs.y - gfxWorld.mins.y);
        const auto yScale = -(yRecip * gfxWorld.mins.y);

        const auto zRecip = 1.0f / (gfxWorld.maxs.z - gfxWorld.mins.z);
        const auto zScale = -(zRecip * gfxWorld.mins.z);

        memset(gfxWorld.outdoorLookupMatrix, 0, sizeof(gfxWorld.outdoorLookupMatrix));

        gfxWorld.outdoorLookupMatrix[0].x = xRecip;
        gfxWorld.outdoorLookupMatrix[1].y = yRecip;
        gfxWorld.outdoorLookupMatrix[2].z = zRecip;
        gfxWorld.outdoorLookupMatrix[3].x = xScale;
        gfxWorld.outdoorLookupMatrix[3].y = yScale;
        gfxWorld.outdoorLookupMatrix[3].z = zScale;
        gfxWorld.outdoorLookupMatrix[3].w = 1.0f;

        const auto outdoorImageAsset = m_context.LoadDependency<AssetImage>("$outdoor");
        if (!outdoorImageAsset)
        {
            con::error("ERROR! unable to find outdoor image $outdoor!");
            return false;
        }
        gfxWorld.outdoorImage = outdoorImageAsset->Asset();

        return true;
    }

    GfxWorld* GfxWorldLinker::LinkGfxWorld(const BSPData& bsp) const
    {
        GfxWorld* gfxWorld = m_memory.Alloc<GfxWorld>();
        gfxWorld->baseName = m_memory.Dup(bsp.name.c_str());
        gfxWorld->name = m_memory.Dup(bsp.bspName.c_str());

        // Default values taken from official maps
        gfxWorld->lightingFlags = 0;
        gfxWorld->lightingQuality = 4096;

        CleanGfxWorld(*gfxWorld);

        // Converted vision grades share a native BO2 LUT atlas. Its material
        // must be bound on the world, not merely listed as a zone asset.
        const auto visionFile = m_search_path.Open(GetFileNameForBSPAsset("visions.json"));
        if (visionFile.IsOpen())
        {
            try
            {
                const auto data = json::parse(*visionFile.m_stream);
                const auto name = data.at("material").get<std::string>();
                const auto material = m_context.LoadDependency<AssetMaterial>(name);
                if (!material)
                {
                    con::error("Could not load vision LUT material {}", name);
                    return nullptr;
                }
                gfxWorld->lutMaterial = material->Asset();
            }
            catch (const json::exception& e)
            {
                con::error("JSON error when parsing visions.json: {}", e.what());
                return nullptr;
            }
        }

        if (!LoadMapSurfaces(bsp, *gfxWorld))
            return nullptr;

        if (!LoadXModels(bsp, *gfxWorld))
            return nullptr;

        if (!LoadLightmapData(*gfxWorld))
            return nullptr;

        LoadSkyBox(bsp, *gfxWorld);

        if (!LoadReflectionProbeData(*gfxWorld))
            return nullptr;

        // world bounds are based on loaded surface mins/maxs
        LoadWorldBounds(*gfxWorld);

        if (!LoadOutdoors(*gfxWorld))
            return nullptr;

        // gfx cells depend on surface/smodel count
        LoadGfxCells(*gfxWorld);

        LoadGfxLights(*gfxWorld);
        LoadShadowGeometry(bsp, *gfxWorld);
        if (!LoadLightGrid(*gfxWorld)) return nullptr;

        LoadModels(*gfxWorld);

        LoadSunData(*gfxWorld);

        LoadDynEntData(*gfxWorld);

        return gfxWorld;
    }
} // namespace BSP

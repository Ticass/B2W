#include <bit>
#include "MapEntsDumperT6.h"

#include "Game/T6/GameAssetPoolT6.h"

#include <cmath>
#include <functional>
#include <limits>
#include <array>
#include <vector>
#include <map>
#include <string>
#include <format>
#include <ostream>
#include <nlohmann/json.hpp>

using namespace T6;

#define OUTF(...) do { out << std::format(__VA_ARGS__); out.flush(); } while (0)

namespace
{
    void DumpLightGrid(std::ostream& out, const GfxLightGrid& grid)
    {
        const auto write = [&out](const auto& value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        out.write("W2BT6LG1", 8);
        const uint32_t version = 1, regions = 0;
        const uint32_t rows = grid.rowDataStart ? grid.maxs[grid.rowAxis] - grid.mins[grid.rowAxis] + 1u : 0u;
        write(version); write(regions); write(grid.sunPrimaryLightIndex); write(grid.mins); write(grid.maxs);
        write(grid.rowAxis); write(grid.colAxis); write(rows); write(grid.rawRowDataSize); write(grid.entryCount); write(grid.colorCount);
        out.write(reinterpret_cast<const char*>(grid.rowDataStart), rows * 2u);
        out.write(reinterpret_cast<const char*>(grid.rawRowData), grid.rawRowDataSize);
        out.write(reinterpret_cast<const char*>(grid.entries), grid.entryCount * 4u);
        out.write(reinterpret_cast<const char*>(grid.colors), grid.colorCount * 168u);
    }
    void DumpCollisionGeometry(std::ostream& out, const clipMap_t& cm)
    {
        using nlohmann::json;
        json data;
        const auto vec = [](const vec3_t& v) { return json::array({v.x, v.y, v.z}); };
        data["vertices"] = json::array();
        data["triangles"] = json::array();
        data["materials"] = json::array();
        data["partitions"] = json::array();
        data["aabbs"] = json::array();
        for (unsigned i = 0; i < cm.vertCount; ++i)
            data["vertices"].push_back(vec(cm.verts[i]));
        for (int i = 0; i < cm.triCount; ++i)
            data["triangles"].push_back(json::array({cm.triIndices[i][0], cm.triIndices[i][1], cm.triIndices[i][2]}));
        for (unsigned i = 0; i < cm.info.numMaterials; ++i)
            data["materials"].push_back({{"name", cm.info.materials[i].name}, {"contents", cm.info.materials[i].contentFlags},
                {"surface_flags", cm.info.materials[i].surfaceFlags}});
        for (int i = 0; i < cm.partitionCount; ++i)
            data["partitions"].push_back(json::array({cm.partitions[i].firstTri, static_cast<unsigned char>(cm.partitions[i].triCount)}));
        for (int i = 0; i < cm.aabbTreeCount; ++i)
        {
            const auto& a = cm.aabbTrees[i];
            data["aabbs"].push_back({{"origin", vec(a.origin)}, {"half_size", vec(a.halfSize)},
                {"material", a.materialIndex}, {"children", a.childCount}, {"index", a.u.partitionIndex}});
        }
        out << data.dump();
    }
    // Diagnostic: plain-text layout of the zone's clipmap next to the .ents
    // file (waw2bo2 compares stock and bridge-built collision with it).
    void DumpClipMapLayout(std::ostream& out, const clipMap_t& cm)
    {
        const auto v3 = [](const vec3_t& v) { return std::format("({:.2f} {:.2f} {:.2f})", v.x, v.y, v.z); };
        const auto& info = cm.info;
        OUTF("planes {} materials {} brushSides {} leafbrushNodes {} leafBrushes {} brushVerts {} nuinds {} brushes {}\n",
                           info.planeCount, info.numMaterials, info.numBrushSides, info.leafbrushNodesCount, info.numLeafBrushes, info.numBrushVerts,
                           info.nuinds, info.numBrushes);
        OUTF("nodes {} leafs {} submodels {} vertCount {} triCount {} partitions {} aabbTrees {} staticModels {} clusters {}\n",
                           cm.numNodes, cm.numLeafs, cm.numSubModels, cm.vertCount, cm.triCount, cm.partitionCount, cm.aabbTreeCount,
                           cm.numStaticModels, cm.numClusters);

        for (auto i = 0u; i < info.numMaterials && i < 6; i++)
            OUTF("material {} '{}' sflags 0x{:X} cflags 0x{:X}\n", i, info.materials[i].name, info.materials[i].surfaceFlags,
                               info.materials[i].contentFlags);

        auto sidesAxial = 0u, sidesTotal = 0u;
        for (auto b = 0u; b < info.numBrushes; b++)
        {
            const auto& brush = info.brushes[b];
            for (auto s = 0u; s < brush.numsides; s++, sidesTotal++)
            {
                const auto& n = brush.sides[s].plane->normal;
                if ((std::fabs(n.x) == 1.0f) + (std::fabs(n.y) == 1.0f) + (std::fabs(n.z) == 1.0f) == 1)
                    sidesAxial++;
            }
        }
        OUTF("brush sides total {} axial {}\n", sidesTotal, sidesAxial);
        OUTF("pInfo {} &info {} isInUse {} checksum {} cmodel0.info {} cmodel1.info {}\n", static_cast<const void*>(cm.pInfo),
             static_cast<const void*>(&cm.info), static_cast<int>(cm.isInUse), cm.checksum, static_cast<const void*>(cm.cmodels[0].info),
             cm.numSubModels > 1 ? static_cast<const void*>(cm.cmodels[1].info) : nullptr);
        OUTF("brushes ptr {} brushBounds ptr {} brushContents ptr {} brushsides ptr {} leafbrushes ptr {}\n", static_cast<const void*>(info.brushes),
             static_cast<const void*>(info.brushBounds), static_cast<const void*>(info.brushContents), static_cast<const void*>(info.brushsides),
             static_cast<const void*>(info.leafbrushes));
        for (auto b = 0u; b < info.numBrushes && b < 4 && info.brushBounds && info.brushContents; b++)
        {
            const auto& brush = info.brushes[b];
            OUTF("brush {} mins {} maxs {} contents 0x{:X} numsides {} numverts {} axialC [{:X} {:X} {:X} / {:X} {:X} {:X}] bounds mid {} half {} contentsArr 0x{:X}\n",
                               b, v3(brush.mins), v3(brush.maxs), static_cast<unsigned>(brush.contents), brush.numsides, brush.numverts,
                               brush.axial_cflags[0][0], brush.axial_cflags[0][1], brush.axial_cflags[0][2], brush.axial_cflags[1][0],
                               brush.axial_cflags[1][1], brush.axial_cflags[1][2], v3(info.brushBounds[b].midPoint), v3(info.brushBounds[b].halfSize),
                               static_cast<unsigned>(info.brushContents[b]));
            for (auto s = 0u; s < brush.numsides && s < 8; s++)
                OUTF("   side {} n {} d {:.2f} type {} sign {} cflags 0x{:X} sflags 0x{:X}\n", s, v3(brush.sides[s].plane->normal),
                                   brush.sides[s].plane->dist, static_cast<int>(brush.sides[s].plane->type),
                                   static_cast<int>(brush.sides[s].plane->signbits), static_cast<unsigned>(brush.sides[s].cflags),
                                   static_cast<unsigned>(brush.sides[s].sflags));
        }
        for (auto n = 0u; n < info.leafbrushNodesCount && n < 24; n++)
        {
            const auto& node = info.leafbrushNodes[n];
            if (node.leafBrushCount > 0)
                OUTF("lbnode {} axis {} count {} contents 0x{:X} first brush {}\n", n, static_cast<int>(node.axis), node.leafBrushCount,
                                   static_cast<unsigned>(node.contents), node.data.leaf.brushes ? node.data.leaf.brushes[0] : -1);
            else
                OUTF("lbnode {} axis {} count {} contents 0x{:X} dist {:.2f} range {:.2f} c0 {} c1 {}\n", n, static_cast<int>(node.axis),
                                   node.leafBrushCount, static_cast<unsigned>(node.contents), node.data.children.dist, node.data.children.range,
                                   node.data.children.childOffset[0], node.data.children.childOffset[1]);
        }
        // Validate the native position-trace partition contract on serialized data.
        // Range is ignored by position traces; shared (+1) subtrees inherit bounds.
        size_t violations = 0, references = 0;
        std::vector<bool> seen(info.numBrushes);
        std::function<void(unsigned int, std::array<float, 3>, std::array<float, 3>, unsigned int)> validate;
        validate = [&](const unsigned int index, auto lo, auto hi, const unsigned int depth)
        {
            if (index >= info.leafbrushNodesCount || depth > info.leafbrushNodesCount)
            {
                violations++;
                return;
            }
            const auto& node = info.leafbrushNodes[index];
            if (node.leafBrushCount <= 0 &&
                (!node.data.children.childOffset[0] || !node.data.children.childOffset[1]))
            {
                violations++;
                return;
            }
            if (node.leafBrushCount > 0)
            {
                for (auto i = 0; i < node.leafBrushCount; i++)
                {
                    const auto id = node.data.leaf.brushes[i];
                    if (id >= info.numBrushes) { violations++; continue; }
                    references++;
                    seen[id] = true;
                    const auto& brush = info.brushes[id];
                    for (auto a = 0; a < 3; a++)
                        if (brush.mins.v[a] < lo[a] || brush.maxs.v[a] > hi[a])
                            violations++;
                }
                return;
            }
            if (node.leafBrushCount < 0)
                validate(index + 1, lo, hi, depth + 1);
            const auto axis = static_cast<unsigned char>(node.axis);
            if (axis > 2) { violations++; return; }
            auto frontLo = lo, backHi = hi;
            frontLo[axis] = std::max(frontLo[axis], node.data.children.dist);
            backHi[axis] = std::min(backHi[axis], node.data.children.dist);
            validate(index + node.data.children.childOffset[0], frontLo, hi, depth + 1);
            validate(index + node.data.children.childOffset[1], lo, backHi, depth + 1);
        };
        const auto inf = std::numeric_limits<float>::infinity();
        if (cm.numLeafs)
            validate(cm.leafs[0].leafBrushNode, {-inf, -inf, -inf}, {inf, inf, inf}, 0);
        OUTF("brush tree position contract violations {} references {} unique {}\n", violations, references,
             std::count(seen.begin(), seen.end(), true));
        auto leafsWithBrushes = 0u, leafsWithAabbs = 0u;
        for (auto l = 0u; l < cm.numLeafs; l++)
        {
            leafsWithBrushes += cm.leafs[l].leafBrushNode != 0;
            leafsWithAabbs += cm.leafs[l].collAabbCount != 0;
        }
        OUTF("leafs with brush node {} with aabbs {}\n", leafsWithBrushes, leafsWithAabbs);
        // Static models are sector-linked and traced against their xmodel collSurfs.
        auto smodelsWithCollSurfs = 0u, smodelsSolid = 0u;
        for (auto m = 0u; m < cm.numStaticModels; m++)
        {
            const auto& sm = cm.staticModelList[m];
            smodelsWithCollSurfs += sm.xmodel && sm.xmodel->collSurfs && sm.xmodel->numCollSurfs > 0;
            smodelsSolid += (sm.contents & 1) != 0;
        }
        OUTF("static models {} with collSurfs {} solid {}\n", cm.numStaticModels, smodelsWithCollSurfs, smodelsSolid);
        for (auto m = 0u; m < cm.numStaticModels && m < 4; m++)
        {
            const auto& sm = cm.staticModelList[m];
            OUTF("smodel {} {} contents 0x{:X} origin {} absmin {} absmax {} collSurfs {}\n", m, sm.xmodel && sm.xmodel->name ? sm.xmodel->name : "<null>",
                 static_cast<unsigned>(sm.contents), v3(sm.origin), v3(sm.absmin), v3(sm.absmax), sm.xmodel ? sm.xmodel->numCollSurfs : -1);
        }
        for (auto l = 0u; l < cm.numLeafs && l < 16; l++)
        {
            const auto& leaf = cm.leafs[l];
            OUTF("leaf {} firstAabb {} aabbCount {} brushContents 0x{:X} terrainContents 0x{:X} mins {} maxs {} brushNode {} cluster {}\n", l,
                               leaf.firstCollAabbIndex, leaf.collAabbCount, static_cast<unsigned>(leaf.brushContents),
                               static_cast<unsigned>(leaf.terrainContents), v3(leaf.mins), v3(leaf.maxs), leaf.leafBrushNode, leaf.cluster);
        }
        for (auto n = 0u; n < cm.numNodes && n < 6; n++)
            OUTF("node {} plane n {} d {:.2f} type {} children {} {}\n", n, v3(cm.nodes[n].plane->normal), cm.nodes[n].plane->dist,
                               static_cast<int>(cm.nodes[n].plane->type), cm.nodes[n].children[0], cm.nodes[n].children[1]);
        for (auto m = 0u; m < static_cast<unsigned>(cm.numSubModels) && m < 4; m++)
        {
            const auto& model = cm.cmodels[m];
            OUTF("cmodel {} mins {} maxs {} radius {:.1f} leaf aabb {}+{} brushContents 0x{:X} terrainContents 0x{:X} leafmins {} leafmaxs {} brushNode {}\n",
                               m, v3(model.mins), v3(model.maxs), model.radius, model.leaf.firstCollAabbIndex, model.leaf.collAabbCount,
                               static_cast<unsigned>(model.leaf.brushContents), static_cast<unsigned>(model.leaf.terrainContents), v3(model.leaf.mins),
                               v3(model.leaf.maxs), model.leaf.leafBrushNode);
        }
        {
            // partition shape statistics (triangles per partition, extent, contiguity)
            std::map<int, int> triHistogram;
            float maxExtent = 0.0f, sumExtent = 0.0f;
            auto borders = 0;
            for (auto p = 0; p < cm.partitionCount; p++)
            {
                const auto& part = cm.partitions[p];
                triHistogram[static_cast<int>(part.triCount)]++;

                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (auto t = 0; t < part.triCount; t++)
                    for (auto c = 0; c < 3; c++)
                    {
                        const auto& v = cm.verts[cm.triIndices[part.firstTri + t][c]];
                        for (auto a = 0; a < 3; a++)
                        {
                            lo[a] = std::min(lo[a], v.v[a]);
                            hi[a] = std::max(hi[a], v.v[a]);
                        }
                    }
                const auto extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
                maxExtent = std::max(maxExtent, extent);
                sumExtent += extent;
            }
            std::string histogram;
            for (const auto& [tris, count] : triHistogram)
                histogram += std::format("{}:{} ", tris, count);
            OUTF("partitions {} tris {} borders {} mean extent {:.1f} max extent {:.1f}\npartition tri histogram {}\n", cm.partitionCount, cm.triCount,
                 borders, cm.partitionCount ? sumExtent / cm.partitionCount : 0.0f, maxExtent, histogram);
            std::map<int, int> childHistogram;
            for (auto a = 0; a < cm.aabbTreeCount; a++)
                if (cm.aabbTrees[a].childCount)
                    childHistogram[cm.aabbTrees[a].childCount]++;
            std::string children;
            for (const auto& [count, n] : childHistogram)
                children += std::format("{}:{} ", count, n);
            OUTF("aabb parent child-count histogram {}\n", children);
            // triangles per clip material contents (leaf aabbs -> partitions)
            std::map<unsigned, int> trisByContents;
            for (auto a = 0; a < cm.aabbTreeCount; a++)
            {
                const auto& tree = cm.aabbTrees[a];
                if (tree.childCount || tree.materialIndex >= cm.info.numMaterials)
                    continue;
                trisByContents[static_cast<unsigned>(cm.info.materials[tree.materialIndex].contentFlags)] +=
                    cm.partitions[tree.u.partitionIndex].triCount;
            }
            std::string byContents;
            for (const auto& [contents, tris] : trisByContents)
                byContents += std::format("0x{:X}:{} ", contents, tris);
            OUTF("clip materials {} terrain triangles by contents {}\n", cm.info.numMaterials, byContents);
        }
        for (auto p = 0; p < cm.partitionCount && p < 4; p++)
            OUTF("partition {} triCount {} firstTri {} nuinds {} fuind {}\n", p, static_cast<int>(cm.partitions[p].triCount),
                               cm.partitions[p].firstTri, cm.partitions[p].nuinds, cm.partitions[p].fuind);
        for (auto a = 0u; a < static_cast<unsigned>(cm.aabbTreeCount) && a < 6; a++)
            OUTF("aabb {} origin {} half {} material {} childCount {} u {}\n", a, v3(cm.aabbTrees[a].origin), v3(cm.aabbTrees[a].halfSize),
                               cm.aabbTrees[a].materialIndex, cm.aabbTrees[a].childCount, cm.aabbTrees[a].u.firstChildIndex);
        if (cm.triCount && cm.triEdgeIsWalkable)
        {
            auto walkable = 0u;
            const auto bits = static_cast<unsigned>(cm.triCount) * 3u;
            for (auto e = 0u; e < bits; e++)
                walkable += (cm.triEdgeIsWalkable[e >> 3] >> (e & 7)) & 1;
            OUTF("walkable edges {} of {}\n", walkable, bits);
        }
        if (cm.triCount && cm.verts && cm.triIndices)
        {
            // winding census: normal = (v1-v0) x (v2-v0); count steep up/down facing triangles
            auto up = 0u, down = 0u, wall = 0u;
            for (auto t = 0; t < cm.triCount; t++)
            {
                const auto& a = cm.verts[cm.triIndices[t][0]];
                const auto& b = cm.verts[cm.triIndices[t][1]];
                const auto& c = cm.verts[cm.triIndices[t][2]];
                const float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
                const float e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const auto len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len <= 0.0f)
                    continue;
                const auto nz = n[2] / len;
                if (nz > 0.7f)
                    up++;
                else if (nz < -0.7f)
                    down++;
                else
                    wall++;
            }
            OUTF("tri winding (v1-v0)x(v2-v0): up {} down {} steep {}\n", up, down, wall);
        }
        OUTF("box_brush contents 0x{:X} numsides {} box_model leaf brushContents 0x{:X} brushNode {}\n",
                           static_cast<unsigned>(cm.box_brush->contents), cm.box_brush->numsides, static_cast<unsigned>(cm.box_model.leaf.brushContents),
                           cm.box_model.leaf.leafBrushNode);
    }
    // Diagnostic: path nodes of a gameworld
    void DumpPathLayout(std::ostream& out, const PathData& path, const ZoneScriptStrings& strings)
    {
        const auto str = [&strings](const unsigned idx) -> std::string { return idx && idx < strings.Count() ? strings.Value(idx) : std::string(); };
        OUTF("nodeCount {} originalNodeCount {} visBytes {} smoothBytes {} nodeTreeCount {}\n", path.nodeCount, path.originalNodeCount, path.visBytes,
             path.smoothBytes, path.nodeTreeCount);
        std::map<int, int> types;
        size_t links = 0;
        for (auto i = 0u; i < path.nodeCount; i++)
        {
            types[path.nodes[i].constant.type]++;
            links += path.nodes[i].constant.totalLinkCount;
        }
        for (const auto& [t, n] : types)
            OUTF("type {} count {}\n", t, n);
        OUTF("total links {}\n", links);
        for (auto i = 0u; i < path.nodeCount && i < 30; i++)
        {
            const auto& c = path.nodes[i].constant;
            const auto& d = path.nodes[i].dynamic;
            OUTF("node {} type {} sf 0x{:X} tn '{}' ln '{}' nw '{}' tg '{}' as '{}' asf {} org ({:.1f} {:.1f} {:.1f}) ang {:.1f} fwd ({:.2f} {:.2f}) r {:.1f} minUse {:.1f} ovl {} {} links {} dyn wLink {} wOvl {} turret {} users {} bad {} base ({:.1f} {:.1f} {:.1f}) btype {}\n", i,
                 static_cast<int>(c.type), c.spawnflags, str(c.targetname), str(c.script_linkName), str(c.script_noteworthy), str(c.target), str(c.animscript),
                 c.animscriptfunc, c.vOrigin.x, c.vOrigin.y, c.vOrigin.z, c.fAngle, c.forward.x, c.forward.y, c.fRadius, c.minUseDistSq, c.wOverlapNode[0],
                 c.wOverlapNode[1], c.totalLinkCount, d.wLinkCount, d.wOverlapCount, d.turretEntNumber, d.userCount, d.hasBadPlaceLink,
                 path.basenodes[i].vOrigin.x, path.basenodes[i].vOrigin.y, path.basenodes[i].vOrigin.z, path.basenodes[i].type);
            for (auto l = 0u; l < c.totalLinkCount && l < 4; l++)
            {
                const auto& link = c.Links[l];
                OUTF("   link node {} dist {:.1f} disc {} neg {} flags {} bad [{} {} {} {} {}]\n", link.nodeNum, link.fDist, static_cast<int>(link.disconnectCount),
                     static_cast<int>(link.negotiationLink), static_cast<int>(link.flags), static_cast<int>(link.ubBadPlaceCount[0]),
                     static_cast<int>(link.ubBadPlaceCount[1]), static_cast<int>(link.ubBadPlaceCount[2]), static_cast<int>(link.ubBadPlaceCount[3]),
                     static_cast<int>(link.ubBadPlaceCount[4]));
            }
        }
        for (auto i = path.nodeCount; i < path.nodeCount + 2; i++)
            OUTF("extra node {} type {} base type {}\n", i, static_cast<int>(path.nodes[i].constant.type), path.basenodes[i].type);
        for (auto t = 0; t < path.nodeTreeCount && t < 12; t++)
        {
            const auto& tree = path.nodeTree[t];
            if (tree.axis < 0)
                OUTF("tree {} leaf count {}\n", t, tree.u.s.nodeCount);
            else
                OUTF("tree {} axis {} dist {:.1f} children {} {}\n", t, tree.axis, tree.dist, tree.u.child[0] - path.nodeTree, tree.u.child[1] - path.nodeTree);
        }
        // full tree + node origins for offline verification
        for (auto t = 0; t < path.nodeTreeCount; t++)
        {
            const auto& tree = path.nodeTree[t];
            if (tree.axis < 0)
            {
                OUTF("T {} L", t);
                for (auto k = 0; k < tree.u.s.nodeCount; k++)
                    OUTF(" {}", tree.u.s.nodes[k]);
                OUTF("\n");
            }
            else
                OUTF("T {} N {} {:.3f} {} {}\n", t, tree.axis, tree.dist, tree.u.child[0] - path.nodeTree, tree.u.child[1] - path.nodeTree);
        }
        for (auto i = 0u; i < path.nodeCount; i++)
            OUTF("O {} {:.3f} {:.3f} {:.3f}\n", i, path.nodes[i].constant.vOrigin.x, path.nodes[i].constant.vOrigin.y, path.nodes[i].constant.vOrigin.z);
        if (path.pathVis && path.visBytes)
        {
            auto ones = 0u;
            for (auto b = 0; b < path.visBytes; b++)
                ones += std::popcount(static_cast<unsigned char>(path.pathVis[b]));
            OUTF("pathVis set bits {} of {} (n*(n+1)/2 = {}, n*n = {})\n", ones, path.visBytes * 8, path.nodeCount * (path.nodeCount + 1) / 2, path.nodeCount * path.nodeCount);
        }
        if (path.smoothCache && path.smoothBytes)
        {
            OUTF("smoothCache first bytes:");
            for (auto b = 0; b < path.smoothBytes && b < 48; b++)
                OUTF(" {:02X}", static_cast<unsigned char>(path.smoothCache[b]));
            OUTF("\n");
        }
    }
    // Diagnostic: world draw buffers and a sample of surfaces per technique set
    void DumpGfxWorldLayout(std::ostream& out, const GfxWorld& world)
    {
        const auto& grid = world.lightGrid;
        OUTF("primaryLights {} grid sun {} mins {} {} {} maxs {} {} {} axes {} {} entries {} colors {} coeffs {} offset {}\n",
             world.primaryLightCount, grid.sunPrimaryLightIndex, grid.mins[0], grid.mins[1], grid.mins[2], grid.maxs[0], grid.maxs[1], grid.maxs[2],
             grid.rowAxis, grid.colAxis, grid.entryCount, grid.colorCount, grid.coeffCount, grid.offset);
        OUTF("lutMaterial {}\n", world.lutMaterial ? world.lutMaterial->info.name : "<default>");
        const auto& draw = world.draw;
        OUTF("vertexCount {} vertexDataSize0 {} (stride {:.2f}) vertexDataSize1 {} indexCount {} surfaces {} lightmaps {} probes {}\n", draw.vertexCount,
             draw.vertexDataSize0, draw.vertexCount ? static_cast<double>(draw.vertexDataSize0) / draw.vertexCount : 0.0, draw.vertexDataSize1,
             draw.indexCount, world.surfaceCount, draw.lightmapCount, draw.reflectionProbeCount);
        OUTF("dpvs lit {}-{} emissiveOpaque {}-{} emissiveTrans {}-{} litTrans {}-{} staticSurfaceCount {}\n", world.dpvs.litSurfsBegin,
             world.dpvs.litSurfsEnd, world.dpvs.emissiveOpaqueSurfsBegin, world.dpvs.emissiveOpaqueSurfsEnd, world.dpvs.emissiveTransSurfsBegin,
             world.dpvs.emissiveTransSurfsEnd, world.dpvs.litTransSurfsBegin, world.dpvs.litTransSurfsEnd, world.dpvs.staticSurfaceCount);
        {
            auto maxTris = 0, maxVerts = 0;
            for (auto i = 0; i < world.surfaceCount; i++)
            {
                maxTris = std::max(maxTris, static_cast<int>(world.dpvs.surfaces[i].tris.triCount));
                maxVerts = std::max(maxVerts, static_cast<int>(world.dpvs.surfaces[i].tris.vertexCount));
            }
            OUTF("surface maxima: tris {} verts {} smodels {}\n", maxTris, maxVerts, world.dpvs.smodelCount);
        }
        std::map<std::string, int> shown;
        auto withOffset1 = 0;
        for (auto i = 0; i < world.surfaceCount; i++)
        {
            const auto& surf = world.dpvs.surfaces[i];
            if (surf.tris.vertexDataOffset1)
                withOffset1++;
            const std::string techset = surf.material && surf.material->techniqueSet ? surf.material->techniqueSet->name : "?";
            if (shown[techset]++ >= 2)
                continue;
            OUTF("surf {} techset {} mat {} off0 {} off1 {} firstVertex {} vertexCount {} tris {} baseIndex {} lmap {} probe {} light {} flags 0x{:X}\n", i,
                 techset, surf.material ? surf.material->info.name : "?", surf.tris.vertexDataOffset0, surf.tris.vertexDataOffset1, surf.tris.firstVertex,
                 surf.tris.vertexCount, surf.tris.triCount, surf.tris.baseIndex, static_cast<int>(surf.lightmapIndex),
                 static_cast<int>(surf.reflectionProbeIndex), static_cast<int>(surf.primaryLightIndex), static_cast<int>(surf.flags));
            if (surf.tris.vertexDataOffset1 && draw.vd1.data)
            {
                std::string bytes;
                const auto* data = reinterpret_cast<const unsigned char*>(draw.vd1.data) + surf.tris.vertexDataOffset1;
                for (auto b = 0; b < 24; b++)
                    bytes += std::format("{:02X} ", data[b]);
                OUTF("   vd1 bytes: {}\n", bytes);
            }
        }
        OUTF("surfaces with vertexDataOffset1 != 0: {}\n", withOffset1);
        {
            std::map<int, int> perLightmap;
            for (auto i = 0; i < world.surfaceCount; i++)
                perLightmap[static_cast<int>(world.dpvs.surfaces[i].lightmapIndex)]++;
            std::string histogram;
            for (const auto& [index, count] : perLightmap)
                histogram += std::format("{}:{} ", index, count);
            auto withLmap = 0u;
            const auto* vertices = reinterpret_cast<const GfxPackedWorldVertex*>(draw.vd0.data);
            for (auto v = 0u; vertices && v < draw.vertexCount; v++)
                withLmap += vertices[v].lmapCoord.packed != 0;
            OUTF("surfaces per lightmap {}\nvertices with lightmap uv {} of {}\n", histogram, withLmap, draw.vertexCount);
        }
        const auto describeImage = [&](const char* label, const GfxImage* image)
        {
            if (!image)
            {
                OUTF("{}: null\n", label);
                return;
            }
            const auto* loadDef = image->texture.loadDef;
            OUTF("{}: '{}' {}x{}x{} levels {} mapType {} semantic {} category {} streaming {} loadDef format {} flags 0x{:X} size {}\n", label,
                 image->name ? image->name : "?", image->width, image->height, image->depth, static_cast<int>(image->levelCount),
                 static_cast<int>(image->mapType), static_cast<int>(image->semantic), static_cast<int>(image->category), static_cast<int>(image->streaming),
                 loadDef ? loadDef->format : -1, loadDef ? static_cast<int>(loadDef->flags) : -1, loadDef ? loadDef->resourceSize : -1);
        };
        for (auto l = 0; l < draw.lightmapCount; l++)
        {
            describeImage("lightmap primary", draw.lightmaps[l].primary);
            describeImage("lightmap secondary", draw.lightmaps[l].secondary);
            const auto* image = draw.lightmaps[l].secondary;
            const auto* loadDef = image ? image->texture.loadDef : nullptr;
            if (loadDef && loadDef->format == 28 && image->height % 3 == 0)
            {
                // average RGBA of each vertically stacked page (skipping unused black texels)
                const auto* px = reinterpret_cast<const unsigned char*>(loadDef->data);
                const auto pageRows = image->height / 3;
                for (auto page = 0; page < 3; page++)
                {
                    double sum[4] = {};
                    size_t count = 0;
                    for (auto y = page * pageRows; y < (page + 1) * pageRows; y += 4)
                        for (auto x = 0; x < image->width; x += 4)
                        {
                            const auto* p = px + (static_cast<size_t>(y) * image->width + x) * 4;
                            if (!p[0] && !p[1] && !p[2] && !p[3])
                                continue;
                            for (auto c = 0; c < 4; c++)
                                sum[c] += p[c];
                            count++;
                        }
                    if (count)
                        OUTF("   page {} mean RGBA {:.1f} {:.1f} {:.1f} {:.1f} over {} texels\n", page, sum[0] / count, sum[1] / count, sum[2] / count,
                             sum[3] / count, count);
                }
            }
        }
        describeImage("probe0", draw.reflectionProbes ? draw.reflectionProbes[0].reflectionImage : nullptr);
        describeImage("outdoor", world.outdoorImage);
        // metadata of ordinary material images, per texture slot semantic
        std::map<std::string, int> slotSeen;
        for (auto i = 0; i < world.surfaceCount; i++)
        {
            const auto* mat = world.dpvs.surfaces[i].material;
            if (!mat)
                continue;
            for (auto t = 0; t < mat->textureCount; t++)
            {
                const auto& tex = mat->textureTable[t];
                const auto key = std::format("semantic{}", static_cast<int>(tex.semantic));
                if (slotSeen[key]++ >= 2 || !tex.image)
                    continue;
                OUTF("slot {} of {}: ", key, mat->info.name);
                describeImage("image", tex.image);
                OUTF("   picmip {} {} noPicmip {} track {} cardMemory {} baseSize {} streamedPartCount {} part0 levels {} size {} delayLoad {}\n",
                     static_cast<int>(tex.image->picmip.platform[0]), static_cast<int>(tex.image->picmip.platform[1]), tex.image->noPicmip,
                     static_cast<int>(tex.image->track), tex.image->cardMemory.platform[0], tex.image->baseSize,
                     static_cast<int>(tex.image->streamedPartCount), static_cast<unsigned>(tex.image->streamedParts[0].levelCount),
                     static_cast<unsigned>(tex.image->streamedParts[0].levelSize), tex.image->delayLoadPixels);
            }
        }
        OUTF("cells {} smodels {} nodeCount {}\n", world.dpvsPlanes.cellCount, world.dpvs.smodelCount, world.nodeCount);
        for (auto c = 0; c < world.dpvsPlanes.cellCount && c < 3; c++)
        {
            const auto& cell = world.cells[c];
            OUTF("cell {} aabbTreeCount {} portals {} mins {} maxs {}\n", c, cell.aabbTreeCount, cell.portalCount,
                 std::format("({:.0f} {:.0f} {:.0f})", cell.mins.x, cell.mins.y, cell.mins.z), std::format("({:.0f} {:.0f} {:.0f})", cell.maxs.x, cell.maxs.y, cell.maxs.z));
            for (auto a = 0; a < cell.aabbTreeCount && a < 12; a++)
            {
                const auto& t = cell.aabbTree[a];
                OUTF("  aabb {} childCount {} childrenOffset {} surfaceCount {} startSurfIndex {} smodelIndexCount {} size ({:.0f} {:.0f} {:.0f})\n", a,
                     t.childCount, t.childrenOffset, t.surfaceCount, t.startSurfIndex, t.smodelIndexCount, t.maxs.x - t.mins.x, t.maxs.y - t.mins.y,
                     t.maxs.z - t.mins.z);
            }
            auto leaves = 0, maxSurfs = 0, maxModels = 0;
            for (auto a = 0; a < cell.aabbTreeCount; a++)
            {
                const auto& t = cell.aabbTree[a];
                if (!t.childCount)
                {
                    leaves++;
                    maxSurfs = std::max<int>(maxSurfs, t.surfaceCount);
                    maxModels = std::max<int>(maxModels, t.smodelIndexCount);
                }
            }
            OUTF("  leaves {} max surfaces per leaf {} max smodels per leaf {}\n", leaves, maxSurfs, maxModels);
        }
        OUTF("sortedSurfIndex first: {} {} {} {}\n", world.dpvs.sortedSurfIndex[0], world.dpvs.sortedSurfIndex[1], world.dpvs.sortedSurfIndex[2],
             world.dpvs.sortedSurfIndex[3]);
        for (const auto& [techset, count] : shown)
            OUTF("techset {} x{}\n", techset, count);
    }
} // namespace

namespace map_ents
{
    DumperT6::DumperT6(const AssetPool<AssetMapEnts::Type>& pool)
        : AbstractAssetDumper(pool)
    {
    }

    void DumperT6::DumpAsset(AssetDumpingContext& context, const XAssetInfo<AssetMapEnts::Type>& asset)
    {
        const auto* mapEnts = asset.Asset();

        const auto mapEntsFile = context.OpenAssetFile(std::format("{}.ents", mapEnts->name));

        if (!mapEntsFile)
            return;

        auto& stream = *mapEntsFile;
        stream.write(mapEnts->entityString, mapEnts->numEntityChars - 1);

        const auto* pools = dynamic_cast<GameAssetPoolT6*>(context.m_zone.m_pools.get());
        if (pools && pools->m_gfx_world)
        {
            for (const auto* gfxWorld : *pools->m_gfx_world)
            {
                const auto layoutFile = context.OpenAssetFile(std::format("{}.gfxworld.txt", mapEnts->name));
                if (layoutFile)
                    DumpGfxWorldLayout(*layoutFile, *gfxWorld->Asset());
                const auto gridFile = context.OpenAssetFile(std::format("{}.t6lightgrid.bin", mapEnts->name));
                if (gridFile)
                    DumpLightGrid(*gridFile, gfxWorld->Asset()->lightGrid);
            }
        }
        if (pools && pools->m_game_world_mp)
        {
            for (const auto* gameWorld : *pools->m_game_world_mp)
            {
                const auto layoutFile = context.OpenAssetFile(std::format("{}.paths.txt", mapEnts->name));
                if (layoutFile)
                    DumpPathLayout(*layoutFile, gameWorld->Asset()->path, context.m_zone.m_script_strings);
            }
        }
        if (pools && pools->m_clip_map)
        {
            for (const auto* clipMap : *pools->m_clip_map)
            {
                const auto layoutFile = context.OpenAssetFile(std::format("{}.clipmap.txt", mapEnts->name));
                if (layoutFile)
                    DumpClipMapLayout(*layoutFile, *clipMap->Asset());
                const auto geometryFile = context.OpenAssetFile(std::format("{}.collision.json", mapEnts->name));
                if (geometryFile)
                    DumpCollisionGeometry(*geometryFile, *clipMap->Asset());
            }
        }
    }
} // namespace map_ents

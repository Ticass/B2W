#include "Asset/IAssetCreator.h"
#include "Asset/IDefaultAssetCreator.h"
#include "Base64.h"
#include "Game/T6/CommonT6.h"
#include "Game/T6/GameT6.h"
#include "Game/T6/T6.h"
#include "SearchPath/MockSearchPath.h"
#include "Utils/MemoryManager.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <format>
#include <string>
#include <vector>

using namespace T6;

// Declared by the templated Game/T6/XModel/LoaderXModelT6.h, which is generated
// into the build tree and not on this project's include path.
namespace xmodel
{
    std::unique_ptr<AssetCreator<AssetXModel>> CreateLoaderT6(MemoryManager& memory, ISearchPath& searchPath, Zone& zone);
} // namespace xmodel

namespace
{
    // Two bones (j_a root, j_b child), one surface, every vertex on exactly one
    // bone. Verts 0-2 are on j_a, verts 3-5 on j_b. Normals are +z and the UVs
    // follow x and y, u mirrored when uSign is negative.
    std::string MakeTwoBoneGltf(const std::vector<uint16_t>& indices, const float uSign = 1.0f)
    {
        const float positions[6][3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 4}, {1, 0, 4}, {0, 1, 4}};
        const uint8_t joints[6][4]{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
        const float weights[6][4]{{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
        float normals[6][3];
        float uvs[6][2];
        for (auto i = 0u; i < 6u; i++)
        {
            normals[i][0] = 0.0f;
            normals[i][1] = 0.0f;
            normals[i][2] = 1.0f;
            uvs[i][0] = uSign * positions[i][0];
            uvs[i][1] = positions[i][1];
        }

        std::vector<uint8_t> buffer(sizeof(positions) + sizeof(joints) + sizeof(weights) + sizeof(normals) + sizeof(uvs)
                                    + indices.size() * sizeof(uint16_t));
        auto* out = buffer.data();
        std::memcpy(out, positions, sizeof(positions));
        out += sizeof(positions);
        std::memcpy(out, joints, sizeof(joints));
        out += sizeof(joints);
        std::memcpy(out, weights, sizeof(weights));
        out += sizeof(weights);
        std::memcpy(out, normals, sizeof(normals));
        out += sizeof(normals);
        std::memcpy(out, uvs, sizeof(uvs));
        out += sizeof(uvs);
        std::memcpy(out, indices.data(), indices.size() * sizeof(uint16_t));

        const auto jointsOffset = sizeof(positions);
        const auto weightsOffset = jointsOffset + sizeof(joints);
        const auto normalsOffset = weightsOffset + sizeof(weights);
        const auto uvsOffset = normalsOffset + sizeof(normals);
        const auto indicesOffset = uvsOffset + sizeof(uvs);

        return std::format(R"({{
"asset": {{"version": "2.0"}},
"scene": 0,
"scenes": [{{"nodes": [3]}}],
"nodes": [
  {{"name": "j_a", "children": [1]}},
  {{"name": "j_b", "translation": [0.0, 0.0, 4.0]}},
  {{"name": "surf0", "skin": 0, "mesh": 0}},
  {{"name": "skel", "children": [0, 2]}}
],
"materials": [{{"name": ",test_material"}}],
"meshes": [{{"primitives": [{{"attributes": {{"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2, "NORMAL": 4, "TEXCOORD_0": 5}}, "indices": 3, "material": 0, "mode": 4}}]}}],
"skins": [{{"skeleton": 0, "joints": [0, 1]}}],
"accessors": [
  {{"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3", "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 4.0]}},
  {{"bufferView": 1, "componentType": 5121, "count": 6, "type": "VEC4"}},
  {{"bufferView": 2, "componentType": 5126, "count": 6, "type": "VEC4"}},
  {{"bufferView": 3, "componentType": 5123, "count": {}, "type": "SCALAR"}},
  {{"bufferView": 4, "componentType": 5126, "count": 6, "type": "VEC3"}},
  {{"bufferView": 5, "componentType": 5126, "count": 6, "type": "VEC2"}}
],
"bufferViews": [
  {{"buffer": 0, "byteOffset": 0, "byteLength": {}}},
  {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
  {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
  {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
  {{"buffer": 0, "byteOffset": {}, "byteLength": {}}},
  {{"buffer": 0, "byteOffset": {}, "byteLength": {}}}
],
"buffers": [{{"byteLength": {}, "uri": "data:application/octet-stream;base64,{}"}}]
}})",
                           indices.size(),
                           sizeof(positions),
                           jointsOffset,
                           sizeof(joints),
                           weightsOffset,
                           sizeof(weights),
                           indicesOffset,
                           indices.size() * sizeof(uint16_t),
                           normalsOffset,
                           sizeof(normals),
                           uvsOffset,
                           sizeof(uvs),
                           buffer.size(),
                           base64::EncodeBase64(buffer.data(), buffer.size()));
    }

    const XSurface& LoadSingleSurface(MockSearchPath& searchPath, MemoryManager& memory, Zone& zone)
    {
        searchPath.AddFileData("xmodel/test_model.json",
                               R"({"_game": "t6", "_type": "xmodel", "_version": 2, "type": "animated", "flags": 0,
                                   "lightingOriginOffset": {"x": 0.0, "y": 0.0, "z": 0.0}, "lightingOriginRange": 0.5,
                                   "lods": [{"distance": 1000.0, "file": "model_export/test_model_lod0.gltf"}]})");

        AssetCreatorCollection creatorCollection(zone);
        creatorCollection.AddDefaultAssetCreator(std::make_unique<DefaultAssetCreator<AssetMaterial>>(memory));
        IgnoredAssetLookup ignoredAssetLookup;
        AssetCreationContext context(zone, &creatorCollection, &ignoredAssetLookup);

        auto loader = xmodel::CreateLoaderT6(memory, searchPath, zone);
        auto result = loader->CreateAsset("test_model", context);
        REQUIRE(result.HasBeenSuccessful());

        const auto* xmodel = reinterpret_cast<XAssetInfo<XModel>*>(result.GetAssetInfo())->Asset();
        REQUIRE(xmodel->numsurfs == 1);
        return xmodel->surfs[0];
    }

    TEST_CASE("LoaderXModel(T6): Rigid surface keeps one vert list per bone", "[t6][xmodel][assetloader]")
    {
        MockSearchPath searchPath;
        searchPath.AddFileData("model_export/test_model_lod0.gltf", MakeTwoBoneGltf({0, 1, 2, 3, 4, 5}));
        MemoryManager memory;
        Zone zone("MockZone", 0, GameId::T6, GamePlatform::PC);

        const auto& surface = LoadSingleSurface(searchPath, memory, zone);

        CHECK((surface.flags & XSURFACE_FLAG_DEFORMED) == 0);
        REQUIRE(surface.vertListCount == 2);
        CHECK(surface.vertList[0].triCount + surface.vertList[1].triCount == surface.triCount);
        CHECK(surface.vertInfo.vertsBlend == nullptr);
    }

    // T6 draws a rigid surface per vert list, each only its own tris: a triangle
    // across two bones would never be drawn (WaW head LOD1+ neck rings).
    TEST_CASE("LoaderXModel(T6): One-weight surface with a cross-bone triangle is skinned", "[t6][xmodel][assetloader]")
    {
        MockSearchPath searchPath;
        searchPath.AddFileData("model_export/test_model_lod0.gltf", MakeTwoBoneGltf({0, 1, 2, 3, 4, 5, 1, 4, 2}));
        MemoryManager memory;
        Zone zone("MockZone", 0, GameId::T6, GamePlatform::PC);

        const auto& surface = LoadSingleSurface(searchPath, memory, zone);

        CHECK((surface.flags & XSURFACE_FLAG_DEFORMED) != 0);
        CHECK((surface.flags & XSURFACE_FLAG_SKINNED) != 0);
        CHECK(surface.triCount == 3);
        CHECK(surface.vertListCount == 0);
        REQUIRE(surface.vertInfo.vertsBlend != nullptr);
        CHECK(surface.vertInfo.vertCount[0] == 6);
        CHECK(surface.vertInfo.vertCount[1] == 0);
        CHECK(surface.vertInfo.vertCount[2] == 0);
        CHECK(surface.vertInfo.vertCount[3] == 0);
    }

    // binormal = binormalSign * cross(normal, tangent) must follow dP/dv, and the
    // tangent dP/du, in the vertices the game reads (positions and UVs as stored).
    void CheckTangentFrames(const XSurface& surface)
    {
        REQUIRE(surface.triCount > 0);
        for (auto triIndex = 0u; triIndex < surface.triCount; triIndex++)
        {
            const auto& tri = surface.triIndices[triIndex];
            float p[3][3], uv[3][2];
            for (auto corner = 0u; corner < 3u; corner++)
            {
                const auto& vertex = surface.verts0[tri.i[corner]];
                p[corner][0] = vertex.xyz.x;
                p[corner][1] = vertex.xyz.y;
                p[corner][2] = vertex.xyz.z;
                Common::Vec2UnpackTexCoords(vertex.texCoord, uv[corner]);
            }
            const float e1[]{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const float e2[]{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            const float d1[]{uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]};
            const float d2[]{uv[2][0] - uv[0][0], uv[2][1] - uv[0][1]};
            const auto r = d1[0] * d2[1] - d2[0] * d1[1];
            REQUIRE(r != 0.0f);
            float dpdu[3], dpdv[3];
            for (auto axis = 0u; axis < 3u; axis++)
            {
                dpdu[axis] = (e1[axis] * d2[1] - e2[axis] * d1[1]) / r;
                dpdv[axis] = (e2[axis] * d1[0] - e1[axis] * d2[0]) / r;
            }

            for (const auto vertexIndex : tri.i)
            {
                const auto& vertex = surface.verts0[vertexIndex];
                float n[3], t[3];
                Common::Vec3UnpackUnitVec(vertex.normal, n);
                Common::Vec3UnpackUnitVec(vertex.tangent, t);
                const float b[]{vertex.binormalSign * (n[1] * t[2] - n[2] * t[1]),
                                vertex.binormalSign * (n[2] * t[0] - n[0] * t[2]),
                                vertex.binormalSign * (n[0] * t[1] - n[1] * t[0])};
                CHECK(t[0] * dpdu[0] + t[1] * dpdu[1] + t[2] * dpdu[2] > 0.9f);
                CHECK(b[0] * dpdv[0] + b[1] * dpdv[1] + b[2] * dpdv[2] > 0.9f);
            }
        }
    }

    TEST_CASE("LoaderXModel(T6): Tangent frame follows the UVs", "[t6][xmodel][assetloader]")
    {
        MockSearchPath searchPath;
        searchPath.AddFileData("model_export/test_model_lod0.gltf", MakeTwoBoneGltf({0, 1, 2, 3, 4, 5}));
        MemoryManager memory;
        Zone zone("MockZone", 0, GameId::T6, GamePlatform::PC);

        CheckTangentFrames(LoadSingleSurface(searchPath, memory, zone));
    }

    TEST_CASE("LoaderXModel(T6): Tangent frame follows mirrored UVs", "[t6][xmodel][assetloader]")
    {
        MockSearchPath searchPath;
        searchPath.AddFileData("model_export/test_model_lod0.gltf", MakeTwoBoneGltf({0, 1, 2, 3, 4, 5}, -1.0f));
        MemoryManager memory;
        Zone zone("MockZone", 0, GameId::T6, GamePlatform::PC);

        CheckTangentFrames(LoadSingleSurface(searchPath, memory, zone));
    }
} // namespace

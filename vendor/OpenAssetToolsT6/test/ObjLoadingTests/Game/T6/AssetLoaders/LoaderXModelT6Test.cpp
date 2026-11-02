#include "Asset/IAssetCreator.h"
#include "Asset/IDefaultAssetCreator.h"
#include "Base64.h"
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
    // bone. Verts 0-2 are on j_a, verts 3-5 on j_b.
    std::string MakeTwoBoneGltf(const std::vector<uint16_t>& indices)
    {
        const float positions[6][3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 4}, {1, 0, 4}, {0, 1, 4}};
        const uint8_t joints[6][4]{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
        const float weights[6][4]{{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};

        std::vector<uint8_t> buffer(sizeof(positions) + sizeof(joints) + sizeof(weights) + indices.size() * sizeof(uint16_t));
        auto* out = buffer.data();
        std::memcpy(out, positions, sizeof(positions));
        out += sizeof(positions);
        std::memcpy(out, joints, sizeof(joints));
        out += sizeof(joints);
        std::memcpy(out, weights, sizeof(weights));
        out += sizeof(weights);
        std::memcpy(out, indices.data(), indices.size() * sizeof(uint16_t));

        const auto jointsOffset = sizeof(positions);
        const auto weightsOffset = jointsOffset + sizeof(joints);
        const auto indicesOffset = weightsOffset + sizeof(weights);

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
"meshes": [{{"primitives": [{{"attributes": {{"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}}, "indices": 3, "material": 0, "mode": 4}}]}}],
"skins": [{{"skeleton": 0, "joints": [0, 1]}}],
"accessors": [
  {{"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3", "min": [0.0, 0.0, 0.0], "max": [1.0, 1.0, 4.0]}},
  {{"bufferView": 1, "componentType": 5121, "count": 6, "type": "VEC4"}},
  {{"bufferView": 2, "componentType": 5126, "count": 6, "type": "VEC4"}},
  {{"bufferView": 3, "componentType": 5123, "count": {}, "type": "SCALAR"}}
],
"bufferViews": [
  {{"buffer": 0, "byteOffset": 0, "byteLength": {}}},
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
} // namespace

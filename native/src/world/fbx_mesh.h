#pragma once

#include "world/vertices.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace w2b::world
{
    struct FbxSurface
    {
        std::uint32_t firstVertex;
        std::uint32_t baseIndex;
        std::uint32_t triangleCount;
        int layerOffset;
        int layerFormat;
    };

    struct FbxMesh
    {
        std::string material;
        int brushModel;
        int primaryLight;
        int reflectionProbe;
        int lightmap;
        bool hidden;
        bool whiteVertexColor;
        std::vector<FbxSurface> surfaces;
    };

    struct RenderBuffers
    {
        std::span<const Vertex> vertices;
        std::span<const std::uint16_t> indices;
        std::span<const std::uint8_t> layers;
    };

    struct FbxUvSet
    {
        std::string name;
        std::vector<double> values;
    };

    struct FbxGeometry
    {
        std::vector<double> positions, normals, tangents, binormals, colors, uvs, lightmapUvs;
        std::vector<int> polygons;
        std::vector<FbxUvSet> layers;
    };

    FbxGeometry BuildFbxGeometry(const RenderBuffers& buffers, const FbxMesh& mesh);
}

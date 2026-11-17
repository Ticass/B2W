#pragma once

#include "world/vertices.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::world
{
    struct RenderGeometry
    {
        std::vector<Vertex> vertices;
        std::vector<std::uint16_t> indices;
    };

    struct WorldSurface
    {
        std::uint32_t firstVertex;
        std::uint16_t vertexCount;
        std::uint16_t triangleCount;
        std::uint32_t baseIndex;
        std::string material;
        std::uint8_t lightmap;
        int reflectionProbe;
        int primaryLight;
        std::uint8_t flags;
        std::array<float, 3> mins;
        std::array<float, 3> maxs;
        std::uint32_t brushModel = 0;
        std::int32_t layerOffset = 0;
        int layerFormat = 0;
        int cell = -1;
        int index;
        bool shadowCopy = false;
    };

    struct WorldStaticModel
    {
        std::string name;
        std::array<float, 3> origin;
        std::array<float, 9> axis;
        float scale;
        std::uint32_t flags;
        float cullDistance = 0;
        std::uint8_t primaryLight = 1;
        std::uint8_t reflectionProbe = 0;
    };

    struct LightRegionHull
    {
        std::array<float, 9> midpoint;
        std::array<float, 9> halfSize;
        // Axis direction xyz, projected midpoint and projected half-size.
        std::vector<std::array<float, 5>> axes;
    };

    struct ShadowLight
    {
        std::vector<std::uint16_t> surfaces;
        std::vector<std::uint16_t> staticModels;
        std::vector<LightRegionHull> hulls;
    };

    struct RenderWorld
    {
        std::string name;
        std::string baseName;
        std::string skyboxModel;
        RenderGeometry geometry;
        std::vector<WorldSurface> surfaces;
        std::vector<WorldStaticModel> staticModels;
        std::optional<std::uint32_t> brushModelCount;
        std::vector<std::uint8_t> layerData;
        std::vector<ShadowLight> shadowLights;
    };

    RenderWorld ReadRenderWorld(const std::filesystem::path& path, bool compensated);
}

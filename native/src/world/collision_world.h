#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::world
{
    using ClipPoint = std::array<float, 3>;

    struct ClipMaterial
    {
        std::string name;
        std::uint32_t surfaceFlags;
        std::uint32_t contentFlags;
    };

    struct BrushPlane
    {
        ClipPoint normal;
        float distance;
        std::uint32_t material;
    };

    struct ClipBrush
    {
        ClipPoint mins;
        ClipPoint maxs;
        std::uint32_t contents;
        std::vector<BrushPlane> planes;
        std::vector<ClipPoint> vertices;
    };

    struct ClipModel
    {
        ClipPoint mins;
        ClipPoint maxs;
        std::vector<std::uint16_t> brushes;
    };

    struct StaticCollisionSurface
    {
        std::uint32_t contents;
        std::uint32_t surfaceFlags;
        ClipPoint mins;
        ClipPoint maxs;
        std::int32_t bone;
        // Model-space plane and two barycentric coordinate vectors.
        std::vector<std::array<float, 12>> triangles;
    };

    struct ClipStaticModel
    {
        std::string name;
        ClipPoint origin;
        std::array<float, 9> inverseScaledAxis;
        ClipPoint absmin;
        ClipPoint absmax;
        std::uint32_t contents;
        std::vector<StaticCollisionSurface> surfaces;
    };

    struct TerrainNode
    {
        std::uint16_t material;
        std::uint16_t children;
        std::int32_t firstChildOrPartition;
    };

    struct TerrainRoot
    {
        std::uint32_t firstNode;
        std::uint32_t count;
        std::int32_t contents;
    };

    struct TerrainPartition
    {
        std::uint32_t firstTriangle;
        std::uint32_t count;
    };

    struct CollisionWorld
    {
        std::string name;
        std::vector<ClipPoint> vertices;
        std::vector<std::uint16_t> indices;
        std::vector<ClipMaterial> materials;
        std::vector<ClipBrush> brushes;
        std::vector<ClipModel> models;
        std::vector<ClipStaticModel> staticModels;
        std::vector<std::uint16_t> triangleMaterials;
        bool brushOwnershipComplete = false;
        std::vector<std::uint8_t> edgeWalkable;
        std::vector<TerrainNode> terrainNodes;
        std::vector<TerrainRoot> leafRoots;
        std::vector<TerrainPartition> partitions;
        std::optional<std::vector<std::uint32_t>> brushReach;
        std::optional<std::vector<TerrainRoot>> modelRoots;
    };

    CollisionWorld ReadCollisionWorld(const std::filesystem::path& path);
}

#include "world/mesh_merge.h"

#include "core/pytext.h"

#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace w2b::world
{
    namespace
    {
        // Keep draws within stock T6 world surface sizes.
        constexpr std::size_t maxTriangles = 1536;
        constexpr std::size_t maxVertices = 1536;
        constexpr double bucketSize = 1024.0;

        void AppendBucket(std::string& key, float min, float max)
        {
            const auto centre = (static_cast<double>(min) + max) * 0.5;
            const auto bucket = std::floor(centre / bucketSize);
            if (!std::isfinite(bucket))
                throw std::runtime_error("non-finite world surface bounds");
            char text[64];
            const auto [end, error] = std::to_chars(text, text + sizeof(text), bucket == 0 ? 0.0 : bucket,
                                                    std::chars_format::fixed, 0);
            if (error != std::errc{})
                throw std::runtime_error("could not format world surface bucket");
            key += ", ";
            key.append(text, end);
        }

        std::string BucketKey(const WorldSurface& surface, std::string_view material)
        {
            // Mesh IDs are embedded in the visibility and shadow data. Match
            // the original ordering by the string representation of the tuple.
            std::string key(material);
            for (const auto value : {static_cast<int>(surface.lightmap), surface.primaryLight,
                                     surface.reflectionProbe, static_cast<int>(surface.brushModel)})
                key += ", " + std::to_string(value);
            key += surface.shadowCopy ? ", True, " : ", False, ";
            key += std::to_string(surface.cell);
            for (std::size_t axis = 0; axis < 3; ++axis)
                AppendBucket(key, surface.mins[axis], surface.maxs[axis]);
            return key + ')';
        }

        struct VertexCounts
        {
            std::vector<std::uint32_t> seen = std::vector<std::uint32_t>(65'536);
            std::uint32_t generation = 0;

            std::size_t Count(std::span<const std::uint16_t> indices, const WorldSurface& surface)
            {
                const auto count = static_cast<std::size_t>(surface.triangleCount) * 3;
                if (surface.baseIndex + count > indices.size())
                    throw std::runtime_error("surface triangle range exceeds the world index buffer");
                ++generation;
                std::size_t used = 0;
                for (const auto local : indices.subspan(surface.baseIndex, count))
                    if (seen[local] != generation)
                    {
                        seen[local] = generation;
                        ++used;
                    }
                return used;
            }
        };

        FbxMesh MakeMesh(const WorldSurface& surface, const std::set<std::string>& blendMaterials,
                         const std::set<std::string>& shadowMaterials)
        {
            return {surface.material, static_cast<int>(surface.brushModel), surface.primaryLight,
                    surface.reflectionProbe, surface.lightmap,
                    surface.shadowCopy || shadowMaterials.contains(surface.material),
                    blendMaterials.contains(surface.material), {}};
        }
    }

    RenderMeshes MergeSurfaces(const RenderWorld& world, const std::set<std::string>& blendMaterials,
                              const std::set<std::string>& shadowMaterials)
    {
        std::unordered_map<std::string_view, std::string> materialKeys;
        std::map<std::string, std::vector<const WorldSurface*>> buckets;
        for (const auto& surface : world.surfaces)
        {
            const auto [material, added] = materialKeys.try_emplace(surface.material);
            if (added)
                material->second = py::StringRepr(surface.material);
            buckets[BucketKey(surface, material->second)].push_back(&surface);
        }

        RenderMeshes result;
        VertexCounts counts;
        for (const auto& [key, surfaces] : buckets)
        {
            std::size_t triangles = 0;
            std::size_t vertices = 0;
            for (const auto* surface : surfaces)
            {
                const auto used = counts.Count(world.geometry.indices, *surface);
                if (surface == surfaces.front() || triangles + surface->triangleCount > maxTriangles ||
                    vertices + used > maxVertices)
                {
                    result.meshes.push_back(MakeMesh(*surface, blendMaterials, shadowMaterials));
                    result.cells.push_back(surface->cell);
                    triangles = vertices = 0;
                }
                result.meshes.back().surfaces.push_back({surface->firstVertex, surface->baseIndex, surface->triangleCount,
                                                        surface->layerOffset, surface->layerFormat});
                if (!surface->shadowCopy)
                    result.surfaceToMesh.emplace_back(surface->index, static_cast<int>(result.meshes.size() - 1));
                triangles += surface->triangleCount;
                vertices += used;
            }
        }
        return result;
    }
}

#include "world/fbx_mesh.h"
#include "world/layers.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace w2b::world
{
    namespace
    {
        using Vec3 = std::array<double, 3>;

        Vec3 Rotate(const Vec3& v)
        {
            return {v[0], v[2], -v[1]};
        }

        template <std::size_t Size>
        void Append(std::vector<double>& values, const std::array<double, Size>& added)
        {
            values.insert(values.end(), added.begin(), added.end());
        }

        void AppendVertex(FbxGeometry& geometry, const Vertex& vertex, bool whiteVertexColor)
        {
            const auto normal = Rotate(vertex.normal);
            const auto tangent = Rotate(vertex.tangent);
            const double sign = vertex.binormalSign < 0 ? -1.0 : 1.0;
            Append(geometry.positions, Rotate(vertex.xyz));
            Append(geometry.normals, normal);
            Append(geometry.tangents, tangent);
            Append(geometry.binormals, Vec3{
                sign * (normal[1] * tangent[2] - normal[2] * tangent[1]),
                sign * (normal[2] * tangent[0] - normal[0] * tangent[2]),
                sign * (normal[0] * tangent[1] - normal[1] * tangent[0])});
            Append(geometry.colors, whiteVertexColor ? std::array{1.0, 1.0, 1.0, 1.0} : vertex.color);
            Append(geometry.uvs, std::array{vertex.uv[0], 1.0 - vertex.uv[1]});
            Append(geometry.lightmapUvs, std::array{vertex.lightmapUv[0] * 0.5, 1.0 - vertex.lightmapUv[1]});
        }

        // Additional texture coordinates are float2s. Layer normal transforms are
        // four bytes each, packed as two exact integers for BSPCreator.
        class Layers
        {
        public:
            explicit Layers(int format) : format_(format), info_(LayerFormat(format == 0xff ? 0 : format)) {}

            std::vector<FbxUvSet> MakeSets() const
            {
                std::vector<FbxUvSet> sets;
                for (int index = 1; index <= info_.extraUv; ++index)
                    sets.push_back({"LayerUV" + std::to_string(index), {}});
                for (int index = 1; index <= info_.extraNormals; ++index)
                    sets.push_back({"LayerNormal" + std::to_string(index), {}});
                return sets;
            }

            void AppendRecord(std::vector<FbxUvSet>& sets, std::span<const std::uint8_t> buffer,
                              const FbxSurface& surface, std::uint16_t local) const
            {
                if (sets.empty())
                    return;
                if (surface.layerFormat != format_)
                    throw std::runtime_error("mesh mixes world vertex formats");
                const int stride = info_.Stride();
                const auto offset = static_cast<std::int64_t>(surface.layerOffset) + local * stride;
                if (offset < 0 || static_cast<std::uint64_t>(offset + stride) > buffer.size())
                    throw std::runtime_error("mesh vertex layer data exceeds the layer buffer");
                const auto* record = buffer.data() + offset;
                for (int index = 0; index < info_.extraUv; ++index, record += 8)
                {
                    std::array<float, 2> uv;
                    std::memcpy(uv.data(), record, sizeof(uv));
                    Append(sets[index].values, std::array{static_cast<double>(uv[0]), 1.0 - uv[1]});
                }
                for (int index = 0; index < info_.extraNormals; ++index, record += 4)
                    Append(sets[info_.extraUv + index].values,
                           std::array{static_cast<double>(record[0] + 256 * record[1]),
                                      static_cast<double>(record[2] + 256 * record[3])});
            }

        private:
            int format_;
            VertexLayerFormat info_;
        };
    }

    FbxGeometry BuildFbxGeometry(const RenderBuffers& buffers, const FbxMesh& mesh)
    {
        FbxGeometry geometry;
        const Layers layers(buffers.layers.empty() ? 0 : mesh.surfaces.at(0).layerFormat);
        geometry.layers = layers.MakeSets();
        std::unordered_map<std::size_t, int> used;
        for (const auto& surface : mesh.surfaces)
        {
            const auto start = static_cast<std::size_t>(surface.baseIndex);
            const auto count = static_cast<std::size_t>(surface.triangleCount) * 3;
            if (start > buffers.indices.size() || count > buffers.indices.size() - start)
                throw std::runtime_error("mesh triangle range exceeds the world index buffer");
            for (std::size_t i = start; i < start + count; i += 3)
            {
                std::array<int, 3> triangle;
                for (std::size_t corner = 0; corner < triangle.size(); ++corner)
                {
                    const auto local = buffers.indices[i + corner];
                    const auto absolute = static_cast<std::size_t>(surface.firstVertex) + local;
                    if (absolute >= buffers.vertices.size())
                        throw std::runtime_error("mesh vertex index exceeds the world vertex buffer");
                    const auto [entry, added] = used.try_emplace(absolute, static_cast<int>(used.size()));
                    triangle[corner] = entry->second;
                    if (added)
                    {
                        AppendVertex(geometry, buffers.vertices[absolute], mesh.whiteVertexColor);
                        layers.AppendRecord(geometry.layers, buffers.layers, surface, local);
                    }
                }
                // BSPCreator reverses polygons on import.
                geometry.polygons.insert(geometry.polygons.end(), {triangle[0], triangle[2], -triangle[1] - 1});
            }
        }
        return geometry;
    }
}

#pragma once

#include <array>
#include <stdexcept>

namespace w2b::world
{
    struct VertexLayerFormat
    {
        int extraUv;
        int extraNormals;

        int Stride() const { return extraUv * 8 + extraNormals * 4; }
    };

    // MTL_WORLDVERT_TEX_<t>_NRM_<n> has 1–5 texcoords and up to three normals.
    inline VertexLayerFormat LayerFormat(int format)
    {
        static constexpr std::array<VertexLayerFormat, 12> formats{{
            {0, 0}, {1, 0}, {1, 1}, {2, 0}, {2, 1}, {2, 2},
            {3, 0}, {3, 1}, {3, 2}, {4, 0}, {4, 1}, {4, 2}}};
        if (format < 0 || static_cast<std::size_t>(format) >= formats.size())
            throw std::runtime_error("unknown world vertex format");
        return formats[format];
    }
}

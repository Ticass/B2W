#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace w2b::world
{
    class BspReader;

    struct Vertex
    {
        std::array<double, 3> xyz;
        std::array<double, 4> color;
        std::array<double, 2> uv;
        std::array<double, 2> lightmapUv;
        std::array<double, 3> normal;
        std::array<double, 3> tangent;
        double binormalSign;
    };

    std::vector<Vertex> ReadVertices(BspReader& input, std::size_t count, bool compensatedSum);

    // Read WaW's 44-byte vertex records and stream little-endian binary64 fields.
    void DecodeVertices(std::istream& input, std::ostream& output, std::size_t count,
                        bool compensatedSum);
}

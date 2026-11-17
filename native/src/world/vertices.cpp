#include "world/vertices.h"
#include "world/bsp_reader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        static_assert(std::endian::native == std::endian::little);
        using PackedVertex = std::array<std::uint32_t, 11>;
        static_assert(sizeof(PackedVertex) == 44 && sizeof(Vertex) == 144);
        constexpr std::size_t batchSize = 1024;

        double Channel(std::uint32_t packed, unsigned shift)
        {
            return static_cast<double>((packed >> shift) & 0xff) / 255.0;
        }

        std::array<double, 3> UnitVector(std::uint32_t packed, bool compensatedSum)
        {
            const double scale = (static_cast<double>(packed >> 24) + 192.0) / 32385.0;
            const auto component = [packed, scale](unsigned shift)
            {
                return (static_cast<double>((packed >> shift) & 0xff) - 127.0) * scale;
            };
            std::array vector{component(0), component(8), component(16)};
            const double x2 = vector[0] * vector[0];
            const double y2 = vector[1] * vector[1];
            const double z2 = vector[2] * vector[2];
            const double xy = x2 + y2;
            double squared = xy + z2;
            // Python 3.12 and newer accumulate sum(float) with compensation.
            if (compensatedSum)
            {
                const double xyError = (std::max(x2, y2) - xy) + std::min(x2, y2);
                const double zError = (std::max(xy, z2) - squared) + std::min(xy, z2);
                squared += xyError + zError;
            }
            const double length = std::sqrt(squared);
            if (length < 1.0e-8)
                return {0.0, 0.0, 1.0};
            for (auto& value : vector)
                value /= length;
            return vector;
        }

        Vertex Decode(const PackedVertex& packed, bool compensatedSum)
        {
            const auto number = [&packed](std::size_t i) { return std::bit_cast<float>(packed[i]); };
            const auto color = packed[3];
            return {{number(0), number(1), number(2)},
                    {Channel(color, 16), Channel(color, 8), Channel(color, 0), Channel(color, 24)},
                    {number(4), number(5)}, {number(6), number(7)},
                    UnitVector(packed[8], compensatedSum), UnitVector(packed[9], compensatedSum), number(10)};
        }

        template <typename Read, typename Append>
        void DecodeBatches(std::size_t count, bool compensatedSum, Read read, Append append)
        {
            std::array<PackedVertex, batchSize> packed;
            std::array<Vertex, batchSize> decoded;
            while (count)
            {
                const auto size = std::min(count, batchSize);
                read(packed.data(), size * sizeof(PackedVertex));
                for (std::size_t i = 0; i < size; ++i)
                    decoded[i] = Decode(packed[i], compensatedSum);
                append(decoded.data(), size);
                count -= size;
            }
        }
    }

    std::vector<Vertex> ReadVertices(BspReader& input, std::size_t count, bool compensatedSum)
    {
        input.CheckCount(count, sizeof(PackedVertex));
        std::vector<Vertex> vertices;
        vertices.reserve(count);
        DecodeBatches(count, compensatedSum, [&](void* destination, std::size_t bytes)
        {
            input.Exact(destination, bytes);
        }, [&](const Vertex* first, std::size_t size)
        {
            vertices.insert(vertices.end(), first, first + size);
        });
        return vertices;
    }

    void DecodeVertices(std::istream& input, std::ostream& output, std::size_t count, bool compensatedSum)
    {
        DecodeBatches(count, compensatedSum, [&](void* destination, std::size_t bytes)
        {
            if (!input.read(static_cast<char*>(destination), bytes))
                throw std::runtime_error("could not read packed world vertex records");
        }, [&](const Vertex* first, std::size_t size)
        {
            if (!output.write(reinterpret_cast<const char*>(first), size * sizeof(Vertex)))
                throw std::runtime_error("could not write decoded world vertex records");
        });
    }
}

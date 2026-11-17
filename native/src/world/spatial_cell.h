#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace w2b::world
{
    using Cell = std::array<int, 3>;

    struct CellHash
    {
        std::size_t operator()(const Cell& cell) const noexcept
        {
            std::size_t seed = 0;
            for (const auto coordinate : cell)
                seed ^= static_cast<unsigned>(coordinate) + std::size_t{0x9e3779b9} + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    inline int FloorCell(double value)
    {
        const auto rounded = std::floor(value);
        if (!std::isfinite(rounded) || rounded < std::numeric_limits<int>::min() || rounded >= std::numeric_limits<int>::max())
            throw std::runtime_error("collision or grid coordinate outside signed cell range");
        return static_cast<int>(rounded);
    }
}

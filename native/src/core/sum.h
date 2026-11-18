#pragma once

#include <cmath>
#include <span>

namespace w2b::py
{
    inline double Sum(std::span<const double> values)
    {
        // Python's compensated sum for floating-point geometry.
        double high = 0, low = 0;
        for (const auto value : values)
        {
            const auto next = high + value;
            low += std::abs(high) >= std::abs(value) ? (high - next) + value : (value - next) + high;
            high = next;
        }
        return low != 0 && std::isfinite(low) ? high + low : high;
    }
}

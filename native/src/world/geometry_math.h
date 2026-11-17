#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace w2b::world
{
    using Vec3 = std::array<double, 3>;

    inline Vec3 ToVec3(const std::array<float, 3>& v) { return {v[0], v[1], v[2]}; }

    inline Vec3 Subtract(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    }

    inline double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    inline double SumDot(const Vec3& a, const Vec3& b, bool compensated)
    {
        if (!compensated)
            return Dot(a, b);
        double sum = 0, correction = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const auto value = a[i] * b[i];
            const auto total = sum + value;
            correction += std::abs(sum) >= std::abs(value) ? (sum - total) + value : (value - total) + sum;
            sum = total;
        }
        return correction && std::isfinite(correction) ? sum + correction : sum;
    }

    inline std::optional<Vec3> Solve3(const std::array<Vec3, 3>& rows, const Vec3& rhs)
    {
        const auto c12 = Cross(rows[1], rows[2]);
        const auto det = Dot(rows[0], c12);
        if (std::abs(det) < 1e-12)
            return std::nullopt;
        const auto c20 = Cross(rows[2], rows[0]);
        const auto c01 = Cross(rows[0], rows[1]);
        Vec3 solution;
        for (std::size_t i = 0; i < solution.size(); ++i)
            solution[i] = (rhs[0] * c12[i] + rhs[1] * c20[i] + rhs[2] * c01[i]) / det;
        return solution;
    }
}

#include "barriers/floor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace w2b::barriers
{
    namespace
    {
        constexpr auto Infinity = std::numeric_limits<double>::infinity();

        std::array<double, 3> Extend(const world::ClipPoint& point)
        {
            return {point[0], point[1], point[2]};
        }

        std::optional<double> BrushTop(const world::ClipBrush& brush, double x, double y, double ceiling)
        {
            if (x < brush.mins[0] || x > brush.maxs[0] || y < brush.mins[1] || y > brush.maxs[1])
                return std::nullopt;
            double top = Infinity, bottom = -Infinity;
            for (const auto& plane : brush.planes)
            {
                const auto [nx, ny, nz] = plane.normal;
                const auto rest = plane.distance - nx * x - ny * y;
                if (nz > 1e-6)
                    top = std::min(top, rest / nz);
                else if (nz < -1e-6)
                    bottom = std::max(bottom, rest / nz);
                else if (rest < -1e-3)
                    return std::nullopt;
            }
            if (bottom <= top && top <= ceiling)
                return top;
            return std::nullopt;
        }
    }

    Floor::Floor(const world::CollisionWorld& clip)
    {
        triangles_.reserve(clip.indices.size() / 3);
        for (std::size_t i = 0; i + 2 < clip.indices.size(); i += 3)
        {
            const auto& a = clip.vertices.at(clip.indices[i]);
            const auto& b = clip.vertices.at(clip.indices[i + 1]);
            const auto& c = clip.vertices.at(clip.indices[i + 2]);
            const auto [minX, maxX] = std::minmax({a[0], b[0], c[0]});
            const auto [minY, maxY] = std::minmax({a[1], b[1], c[1]});
            triangles_.push_back({minX, maxX, minY, maxY, Extend(a), Extend(b), Extend(c)});
        }
        std::vector<bool> owned(clip.brushes.size());
        for (std::size_t i = 1; i < clip.models.size(); ++i)
            for (const auto brush : clip.models[i].brushes)
                owned.at(brush) = true;
        for (std::size_t i = 0; i < clip.brushes.size(); ++i)
            if (!owned[i] && (clip.brushes[i].contents & 1))
                brushes_.push_back(&clip.brushes[i]);
    }

    std::optional<double> Floor::Below(double x, double y, double ceiling) const
    {
        std::optional<double> best;
        for (const auto& triangle : triangles_)
        {
            if (x < triangle.minX || x > triangle.maxX || y < triangle.minY || y > triangle.maxY)
                continue;
            const auto& a = triangle.a;
            const auto& b = triangle.b;
            const auto& c = triangle.c;
            const auto det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
            if (std::abs(det) <= 1e-9)
                continue;
            const auto u = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
            const auto v = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
            const auto w = 1 - u - v;
            const auto z = u * a[2] + v * b[2] + w * c[2];
            if (std::min({u, v, w}) >= -1e-6 && z <= ceiling && (!best || z > *best))
                best = z;
        }
        for (const auto* brush : brushes_)
            if (const auto top = BrushTop(*brush, x, y, ceiling); top && (!best || *top > *best))
                best = top;
        return best;
    }
}

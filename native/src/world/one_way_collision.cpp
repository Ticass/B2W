#include "world/one_way_collision.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace w2b::world
{
    namespace
    {
        constexpr std::uint32_t playerMask = 0x281c011;
        constexpr std::uint32_t solid = 0x1;
        constexpr double steepNormalZ = 0.7;
        using Vec = std::array<double, 3>;
        using Triangle = std::array<Vec, 3>;

        Vec Subtract(const Vec& a, const Vec& b)
        {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }

        Vec Cross(const Vec& a, const Vec& b)
        {
            return {a[1] * b[2] - a[2] * b[1],
                    a[2] * b[0] - a[0] * b[2],
                    a[0] * b[1] - a[1] * b[0]};
        }

        double Dot(const Vec& a, const Vec& b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        std::optional<Vec> Unit(Vec value)
        {
            const auto length = std::sqrt(Dot(value, value));
            if (length < 1e-6)
                return std::nullopt;
            for (auto& component : value)
                component /= length;
            return value;
        }

        Triangle Corners(const CollisionWorld& world, std::size_t index)
        {
            Triangle triangle;
            for (std::size_t corner = 0; corner < triangle.size(); ++corner)
            {
                const auto& v = world.vertices.at(world.indices.at(index * 3 + corner));
                triangle[corner] = {v[0], v[1], v[2]};
            }
            return triangle;
        }

        std::optional<Vec> FrontNormal(const Triangle& triangle)
        {
            const auto& [a, b, c] = triangle;
            return Unit(Cross(Subtract(c, a), Subtract(b, a)));
        }

        std::optional<Triangle> CornerKey(Triangle triangle)
        {
            for (auto& corner : triangle)
                for (auto& coordinate : corner)
                {
                    if (std::isnan(coordinate))
                        return std::nullopt;
                    // Stored coordinates are binary32. Scaling by ten is
                    // exact in binary64, so ties match round(x, 1).
                    coordinate = std::nearbyint(10.0 * coordinate);
                }
            std::ranges::sort(triangle);
            const auto repeated = std::unique(triangle.begin(), triangle.end());
            std::fill(repeated, triangle.end(), *(repeated - 1));
            return triangle;
        }

        bool HasThreeEdges(const Triangle& triangle, const Vec& normal)
        {
            for (std::size_t corner = 0; corner < triangle.size(); ++corner)
            {
                const auto edge = Subtract(triangle[(corner + 1) % 3], triangle[corner]);
                if (!Unit(Cross(normal, edge)))
                    return false;
            }
            return true;
        }

        struct Candidate
        {
            std::size_t triangle;
            Vec normal;
            bool opposed = false;
        };
    }

    std::vector<std::size_t> OneWayTriangles(const CollisionWorld& world)
    {
        std::vector<Candidate> candidates;
        std::map<Triangle, std::vector<std::size_t>> candidatesByCorners;
        for (std::size_t index = 0; index < world.triangleMaterials.size(); ++index)
        {
            const auto material = world.triangleMaterials[index];
            if (material == 0xffff)
                continue;
            const auto contents = world.materials.at(material).contentFlags;
            if (!(contents & playerMask) || (contents & solid))
                continue;
            const auto triangle = Corners(world, index);
            const auto normal = FrontNormal(triangle);
            if (!normal || std::abs((*normal)[2]) >= steepNormalZ || !HasThreeEdges(triangle, *normal))
                continue;
            if (const auto key = CornerKey(triangle))
                candidatesByCorners[*key].push_back(candidates.size());
            candidates.push_back({index, *normal});
        }

        // Only candidate corner sets need matching. Solid geometry can still
        // provide the opposing face of a candidate.
        if (!candidatesByCorners.empty())
            for (std::size_t index = 0; index < world.triangleMaterials.size(); ++index)
            {
                const auto material = world.triangleMaterials[index];
                if (material == 0xffff || !(world.materials.at(material).contentFlags & playerMask))
                    continue;
                const auto triangle = Corners(world, index);
                const auto key = CornerKey(triangle);
                if (!key)
                    continue;
                const auto matching = candidatesByCorners.find(*key);
                if (matching == candidatesByCorners.end())
                    continue;
                if (const auto normal = FrontNormal(triangle))
                    for (const auto id : matching->second)
                    {
                        auto& candidate = candidates[id];
                        if (candidate.triangle != index && Dot(candidate.normal, *normal) < -0.99)
                            candidate.opposed = true;
                    }
            }

        std::vector<std::size_t> result;
        for (const auto& candidate : candidates)
            if (!candidate.opposed)
                result.push_back(candidate.triangle);
        return result;
    }
}

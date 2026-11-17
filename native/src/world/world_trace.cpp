#include "world/world_trace.h"

#include "world/terrain_ownership.h"

#include <algorithm>

namespace w2b::world
{
    namespace
    {
        template <typename Visit>
        bool VisitCells(const Vec3& min, const Vec3& max, Visit visit)
        {
            constexpr double size = 128;
            Cell low, high;
            for (std::size_t axis = 0; axis < min.size(); ++axis)
            {
                low[axis] = FloorCell(min[axis] / size);
                high[axis] = FloorCell(max[axis] / size);
            }
            for (int x = low[0]; x <= high[0]; ++x)
                for (int y = low[1]; y <= high[1]; ++y)
                    for (int z = low[2]; z <= high[2]; ++z)
                        if (!visit(Cell{x, y, z}))
                            return false;
            return true;
        }

        bool HitsBrush(const ClipBrush& brush, const Vec3& start, const Vec3& end, bool compensated)
        {
            double enter = 0, leave = 1;
            for (const auto& plane : brush.planes)
            {
                const auto normal = ToVec3(plane.normal);
                const auto d0 = SumDot(normal, start, compensated) - plane.distance;
                const auto d1 = SumDot(normal, end, compensated) - plane.distance;
                if (d0 > 0 && d1 > 0)
                    return false;
                if (d0 <= 0 && d1 <= 0)
                    continue;
                const auto intersection = d0 / (d0 - d1);
                if (d0 > 0)
                    enter = std::max(enter, intersection);
                else
                    leave = std::min(leave, intersection);
                if (enter > leave)
                    return false;
            }
            return true;
        }

        bool HitsTriangle(const std::array<Vec3, 3>& triangle, const Vec3& start, const Vec3& end, bool compensated)
        {
            const auto& [a, b, c] = triangle;
            const auto direction = Subtract(end, start);
            const auto e1 = Subtract(b, a), e2 = Subtract(c, a);
            const auto p = Cross(direction, e2);
            const auto det = SumDot(e1, p, compensated);
            if (std::abs(det) < 1e-6)
                return false;
            const auto inv = 1.0 / det;
            const auto s = Subtract(start, a);
            const auto u = SumDot(s, p, compensated) * inv;
            if (u < 0 || u > 1)
                return false;
            const auto q = Cross(s, e1);
            const auto v = SumDot(direction, q, compensated) * inv;
            if (v < 0 || u + v > 1)
                return false;
            const auto t = SumDot(e2, q, compensated) * inv;
            return 0 <= t && t <= 1;
        }
    }

    WorldTracer::WorldTracer(const CollisionWorld& world, std::uint32_t mask, bool compensated)
        : world_(world), compensated_(compensated)
    {
        std::vector<bool> owned(world.brushes.size());
        for (std::size_t model = 1; model < world.models.size(); ++model)
            for (const auto brush : world.models[model].brushes)
                owned.at(brush) = true;
        for (std::size_t index = 0; index < world.brushes.size(); ++index)
        {
            const auto& brush = world.brushes[index];
            const auto reach = world.brushReach ? world.brushReach->at(index) : brush.contents;
            if (!owned[index] && (brush.contents & mask) && (reach & mask))
                Add({true, index}, ToVec3(brush.mins), ToVec3(brush.maxs));
        }
        const auto owners = TerrainOwners(world);
        for (std::size_t tri = 0; tri < world.triangleMaterials.size(); ++tri)
        {
            const auto material = world.triangleMaterials[tri];
            if (owners.at(tri) || material >= world.materials.size() || !(world.materials[material].contentFlags & mask))
                continue;
            Triangle triangle;
            for (std::size_t corner = 0; corner < triangle.size(); ++corner)
                triangle[corner] = ToVec3(world.vertices.at(world.indices.at(3 * tri + corner)));
            Vec3 min, max;
            for (std::size_t axis = 0; axis < min.size(); ++axis)
            {
                min[axis] = std::min({triangle[0][axis], triangle[1][axis], triangle[2][axis]});
                max[axis] = std::max({triangle[0][axis], triangle[1][axis], triangle[2][axis]});
            }
            Add({false, triangles_.size()}, min, max);
            triangles_.push_back(triangle);
        }
    }

    void WorldTracer::Add(Shape shape, const Vec3& min, const Vec3& max)
    {
        VisitCells(min, max, [&](const Cell& cell)
        {
            cells_[cell].push_back(shape);
            return true;
        });
    }

    bool WorldTracer::Visible(const Vec3& start, const Vec3& end) const
    {
        Vec3 min, max;
        for (std::size_t axis = 0; axis < min.size(); ++axis)
        {
            min[axis] = std::min(start[axis], end[axis]);
            max[axis] = std::max(start[axis], end[axis]);
        }
        return VisitCells(min, max, [&](const Cell& cell)
        {
            const auto found = cells_.find(cell);
            if (found == cells_.end())
                return true;
            for (const auto& shape : found->second)
                if (shape.brush ? HitsBrush(world_.brushes[shape.index], start, end, compensated_) :
                                  HitsTriangle(triangles_[shape.index], start, end, compensated_))
                    return false;
            return true;
        });
    }
}

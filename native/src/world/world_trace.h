#pragma once

#include "world/collision_world.h"
#include "world/geometry_math.h"
#include "world/spatial_cell.h"

#include <unordered_map>

namespace w2b::world
{
    class WorldTracer
    {
    public:
        WorldTracer(const CollisionWorld& world, std::uint32_t mask, bool compensated);
        bool Visible(const Vec3& start, const Vec3& end) const;

    private:
        using Triangle = std::array<Vec3, 3>;
        struct Shape { bool brush; std::size_t index; };
        void Add(Shape shape, const Vec3& min, const Vec3& max);

        const CollisionWorld& world_;
        bool compensated_;
        std::vector<Triangle> triangles_;
        std::unordered_map<Cell, std::vector<Shape>, CellHash> cells_;
    };
}

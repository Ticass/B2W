#pragma once

#include "world/collision_world.h"

namespace w2b::barriers
{
    class Floor
    {
    public:
        explicit Floor(const world::CollisionWorld& clip);
        std::optional<double> Below(double x, double y, double ceiling) const;

    private:
        struct Triangle
        {
            float minX, maxX, minY, maxY;
            std::array<double, 3> a, b, c;
        };

        std::vector<Triangle> triangles_;
        std::vector<const world::ClipBrush*> brushes_;
    };
}

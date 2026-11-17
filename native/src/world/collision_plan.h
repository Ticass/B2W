#pragma once

#include "world/collision_world.h"

#include <cstddef>
#include <vector>

namespace w2b::world
{
    struct TerrainSlot
    {
        int material;
        std::uint32_t model;
    };

    struct CollisionPlan
    {
        // -1 leaves a triangle out of the T6 terrain.
        std::vector<int> triangleSlots;
        std::vector<TerrainSlot> slots;
        std::vector<std::size_t> oneWayTriangles;
        std::size_t nonCollidingDropped = 0;
        std::size_t waterDropped = 0;
    };

    CollisionPlan PlanCollision(const CollisionWorld& world, std::optional<std::size_t> triangleLimit = {});
}

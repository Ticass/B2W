#pragma once

#include "world/collision_world.h"

#include <cstddef>
#include <vector>

namespace w2b::world
{
    std::vector<std::size_t> OneWayTriangles(const CollisionWorld& world);
}

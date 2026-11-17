#pragma once

#include "world/collision_world.h"

#include <cstdint>
#include <vector>

namespace w2b::world
{
    // Model 0 is the world. Moving models claim triangles through their
    // authored terrain trees, independently of the models' bounding boxes.
    std::vector<std::uint32_t> TerrainOwners(const CollisionWorld& world);
}

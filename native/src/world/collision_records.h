#pragma once

#include "core/pytext.h"
#include "world/collision_world.h"

namespace w2b::world
{
    struct CollisionRecords
    {
        py::Json records;
        py::Json summary;
    };

    CollisionRecords BuildWorldBrushes(const CollisionWorld& world);
    py::Json BuildSubmodels(const CollisionWorld& world);
    CollisionRecords BuildStaticModels(const CollisionWorld& world);
}

#pragma once

#include "world/collision_plan.h"
#include "world/fbx_mesh.h"

#include <iosfwd>
#include <span>

namespace w2b::world
{
    void WriteWorldFbx(std::ostream& output, const RenderBuffers& buffers, std::span<const FbxMesh> meshes);
    void WriteCollisionFbx(std::ostream& output, const CollisionWorld& world, const CollisionPlan& plan);
}

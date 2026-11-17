#pragma once

#include "world/fbx_mesh.h"
#include "world/render_world.h"

#include <set>
#include <utility>

namespace w2b::world
{
    struct RenderMeshes
    {
        std::vector<FbxMesh> meshes;
        std::vector<int> cells;
        std::vector<std::pair<int, int>> surfaceToMesh;
    };

    RenderMeshes MergeSurfaces(const RenderWorld& world, const std::set<std::string>& blendMaterials = {},
                              const std::set<std::string>& shadowMaterials = {});
}

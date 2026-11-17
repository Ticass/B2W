#pragma once

#include "core/pytext.h"
#include "world/mesh_merge.h"

namespace w2b::world
{
    std::size_t AddShadowCopies(RenderWorld& world, const std::set<std::string>& materials);
    py::Json WriteShadowGeometry(const RenderWorld& world, const RenderMeshes& result, const std::filesystem::path& path);
}

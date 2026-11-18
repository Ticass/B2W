#pragma once

#include "models/models.h"

#include <map>

namespace w2b::models
{
    using MaterialNames = std::set<std::string, std::less<>>;
    using SurfaceOrder = std::vector<std::string>;

    std::size_t OpaqueVertexAlpha(Json& gltf, const MaterialNames& materials);
    SurfaceOrder SurfaceMaterials(const Json& gltf);
    Json ApplySurfaceOrder(const Path& project, const Json& materials, const std::vector<SurfaceOrder>& orders);
    bool HasModelSortMaterials(const Json& materials);
    void ReplaceStagedText(const Path& path, std::string_view text);
    Json StageSurfaceFixes(const Path& project, const Json& materials, bool colors, bool sort);
    Json StageSurfaceFixesRequest(const Path& request);
}

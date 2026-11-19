#pragma once

#include "models/models.h"

#include <map>
#include <optional>

namespace w2b::models
{
    using MaterialNames = std::set<std::string, std::less<>>;
    using SurfaceOrder = std::vector<std::string>;

    // a model material whose T6 sort key was raised to keep the WaW surface order
    struct SortKeyChange
    {
        std::string material;
        int from = 0, to = 0;
    };

    struct SurfaceFixes
    {
        std::vector<std::pair<std::string, std::size_t>> opacity; // glTF -> additive vertices made opaque
        std::vector<SortKeyChange> sortKeys;
        py::Json OpacityJson() const;
        py::Json SortKeysJson() const;
    };

    // a staged material as the surface fixes see it
    struct SurfaceMaterial
    {
        std::string wawName, file;          // the WaW material and its staged JSON, project-relative
        std::string t4Techset;              // empty: not converted from a WaW material
        std::optional<int> t4SortKey;
    };

    std::size_t OpaqueVertexAlpha(Json& gltf, const MaterialNames& materials);
    SurfaceOrder SurfaceMaterials(const Json& gltf);
    std::vector<SortKeyChange> ApplySurfaceOrder(const Path& project, const std::vector<SurfaceMaterial>& materials, const std::vector<SurfaceOrder>& orders);
    bool HasModelSortMaterials(const std::vector<SurfaceMaterial>& materials);
    void ReplaceStagedText(const Path& path, std::string_view text);
    SurfaceFixes StageSurfaceFixes(const Path& project, const std::vector<SurfaceMaterial>& materials, bool colors, bool sort);
}

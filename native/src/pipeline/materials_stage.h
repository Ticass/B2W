#pragma once
// t6bridge.stage_materials, per material (see materials_stage.cpp).

#include "core/pytext.h"
#include "pipeline/material_record.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct MaterialStageRequest
    {
        std::set<std::string> names;
        std::vector<std::filesystem::path> roots;
        std::filesystem::path stockMaterials, projectRoot;
        std::optional<std::filesystem::path> techsetRoot;
        std::string imagePrefix = "waw_world/";
        py::Json donors; // techniqueSet -> stock donor material (bridge::Donors)
        std::optional<std::array<double, 4>> falloffPlacement;
        bool lightShadows = true, layeredVertexData = false;
        std::map<std::string, std::filesystem::path> nativeFallbacks; // material -> BO2 equivalent
        std::map<std::string, std::string> rename;                    // source material -> staged name
    };

    // one material's outcome, in name order
    struct MaterialOutcome
    {
        std::string name;
        bool native = false;
        std::optional<MaterialRecord> entry; // the report's material record; none: no material
        std::string techset;
        py::Json warnings, errors, images;
    };

    std::vector<MaterialOutcome> StageMaterials(const MaterialStageRequest& request);
} // namespace w2b::pipeline

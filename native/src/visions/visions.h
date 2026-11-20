#pragma once
// WaW vision files to neutral T6 visions, the
// native LUT atlas and the full-screen film overlay materials.

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::visions
{
    // vision_float: sscanf("%f") semantics with a numeric prefix accepted
    double VisionFloat(const std::string& value);
    // visions.film_terms: (contrast, brightness, desaturation weight)
    std::tuple<double, double, double> FilmTerms(const util::Json& fields);
    // visions.atlas: the IWI27 RGBA LUT atlas of the grades
    std::string Atlas(const util::Json& grades);
    struct VisionRequest
    {
        std::filesystem::path project;
        std::vector<std::filesystem::path> roots, iwds;
        extract::StockWaw* stock = nullptr;
        std::optional<std::filesystem::path> donor, sourceRoot, techsetDump;
    };

    // visions.stage: the vision report (the LUTs, the glow overlay, the script render dvars)
    util::Json StageVisions(const VisionRequest& request);
} // namespace w2b::visions

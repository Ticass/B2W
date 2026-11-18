#pragma once
// Port of visions.py and glow.py: WaW vision files to neutral T6 visions, the
// native LUT atlas and the full-screen film overlay materials.

#include "core/pytext.h"

#include <filesystem>
#include <string>
#include <tuple>

namespace w2b::visions
{
    // vision_float: sscanf("%f") semantics with a numeric prefix accepted
    double VisionFloat(const std::string& value);
    // visions.film_terms: (contrast, brightness, desaturation weight)
    std::tuple<double, double, double> FilmTerms(const py::Json& fields);
    // visions.atlas: the IWI27 RGBA LUT atlas of the grades
    std::string Atlas(const py::Json& grades);
    // visions.stage; request {project, roots, iwds, stock, source_root, techset_dump}
    py::Json StageVisions(const std::filesystem::path& request);
} // namespace w2b::visions

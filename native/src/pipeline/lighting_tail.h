#pragma once
// t6bridge.stage_bridge, after the light grid: the map's primary light
// definitions with their attenuation ramps, the source lighting copies, and
// the stock T6 window barrier with the map's board repair timing.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, roots, definitions, lighting_base}; returns {images, errors}
    py::Json StageLightDefs(const std::filesystem::path& request);
    // request {project_root, techset_dump, bo2_root, timing}; returns {warnings, errors, content}
    py::Json StageZbarrier(const std::filesystem::path& request);
} // namespace w2b::pipeline

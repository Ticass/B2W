#pragma once
// t6bridge.stage_source_skybox: the map's own sky dome (worldspawn
// skyboxmodel) with its layered materials on the stock BO2 skybox material
// and their WaW shader programs bound (shaderruntime.bind_material).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project, project_root, roots, source_name, native_root, bo2_root};
    // returns {materials (report entries), sky (report content)} or {stage_error}
    py::Json StageSourceSkybox(const std::filesystem::path& request);
} // namespace w2b::pipeline

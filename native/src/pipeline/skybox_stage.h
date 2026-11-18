#pragma once
// The sky of the converted map. t6bridge.stage_source_skybox: the map's own
// sky dome (worldspawn skyboxmodel) with its layered materials on the stock
// BO2 skybox material and their WaW shader programs bound
// (shaderruntime.bind_material). t6bridge.stage_skybox: otherwise BO2's dome
// drawn with the WaW sky cube.

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    // request {project, project_root, roots, source_name, native_root, bo2_root};
    // returns {materials (report entries), sky (report content)} or {stage_error}
    py::Json StageSourceSkybox(const std::filesystem::path& request);

    struct TemplateSky
    {
        std::optional<std::string> techset, image;
    };

    // stage_skybox: BO2's sky dome with the WaW sky cube `skyImage` (found in
    // `imageRoots`), black when the map has no sky; appends to report's
    // warnings and errors
    TemplateSky StageTemplateSkybox(py::Json& report, const std::string& project, const std::filesystem::path& projectRoot,
                                    const std::optional<std::filesystem::path>& bo2Root, std::optional<std::string> skyImage,
                                    const std::vector<std::filesystem::path>& imageRoots);
} // namespace w2b::pipeline

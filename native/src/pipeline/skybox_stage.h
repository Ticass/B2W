#pragma once
// The sky of the converted map. t6bridge.stage_source_skybox: the map's own
// sky dome (worldspawn skyboxmodel) with its layered materials on the stock
// BO2 skybox material and their WaW shader programs bound
// (shaderruntime.bind_material). t6bridge.stage_skybox: otherwise BO2's dome
// drawn with the WaW sky cube.

#include "core/text.h"
#include "pipeline/bridge_report.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct SourceSkyRequest
    {
        std::string project;
        std::filesystem::path projectRoot;
        std::vector<std::filesystem::path> roots;
        std::string sourceName;                   // the worldspawn skyboxmodel
        std::filesystem::path nativeRoot, bo2Root; // the stock T6 dump, the BO2 install
    };

    struct SourceSky
    {
        std::string sourceModel;
        std::vector<std::string> materials;
        util::Json ToJson() const;
    };

    struct SourceSkyStage
    {
        std::vector<MaterialRecord> materials;
        SourceSky sky;
    };

    // Throws StepError("StageError") when the sky's model, geometry, materials or shaders are missing.
    SourceSkyStage StageSourceSkybox(const SourceSkyRequest& request);

    struct TemplateSky
    {
        std::optional<std::string> techset, image;
    };

    // stage_skybox: BO2's sky dome with the WaW sky cube `skyImage` (found in
    // `imageRoots`), black when the map has no sky; appends to report's
    // warnings and errors
    TemplateSky StageTemplateSkybox(BridgeReport& report, const std::string& project, const std::filesystem::path& projectRoot,
                                    const std::optional<std::filesystem::path>& bo2Root, std::optional<std::string> skyImage,
                                    const std::vector<std::filesystem::path>& imageRoots);
} // namespace w2b::pipeline

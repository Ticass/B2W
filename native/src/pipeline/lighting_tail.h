#pragma once
// t6bridge.stage_bridge, after the light grid: the map's primary light
// definitions with their attenuation ramps, the source lighting copies, and
// the stock T6 window barrier with the map's board repair timing.

#include "core/text.h"
#include "pipeline/step_report.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct LightDefs
    {
        std::vector<std::string> images; // attenuation ramps the map zone carries
        std::vector<std::string> errors;
    };

    LightDefs StageLightDefs(const std::filesystem::path& projectRoot, const std::vector<std::filesystem::path>& roots,
                             const std::set<std::string>& definitions);
    // timing: the map's replace_chunk motion (scripts.window_repair_timing), null without one
    StepReport StageZbarrier(const std::filesystem::path& projectRoot, const std::optional<std::filesystem::path>& techsetDump,
                             const std::optional<std::filesystem::path>& bo2Root, const util::Json& timing);
} // namespace w2b::pipeline

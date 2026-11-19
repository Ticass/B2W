#pragma once

#include "pipeline/step_report.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct MapHookRequest
    {
        std::filesystem::path projectRoot;
        std::string project, mapName;
        std::optional<std::filesystem::path> bo2Root;
        bool stockPerks = false, scripts = false, characters = false;
        std::vector<std::string> stagedAnimtrees;
    };

    // The BO2 template main hooked to run the map's own WaW code, and the owned perk support.
    StepReport HookMapScripts(const MapHookRequest& request);
} // namespace w2b::pipeline

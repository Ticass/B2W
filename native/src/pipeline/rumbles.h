#pragma once

#include "core/text.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct RumbleStage
    {
        std::vector<std::string> files;        // staged rumble/ paths
        std::set<std::string> missing;         // referenced, found in no root
        std::set<std::string> nativeProfiles;  // taken from BO2's raw/
        util::Json ToJson() const;
    };

    // Throws StepError("ValueError") on a malformed rumble name or graph.
    RumbleStage StageRumbles(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots,
                             const std::filesystem::path& nativeRaw);
}

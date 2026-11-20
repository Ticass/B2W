#pragma once

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace w2b::weapons
{
    struct WaterBulletRows
    {
        std::vector<std::pair<std::string, double>> depths; // penetrateType -> depth, table order
        std::vector<std::string> weapons;                   // bullet weapons that cross water
        util::Json ToJson() const;
    };

    struct WaterBulletStage
    {
        std::optional<std::string> warning;
        std::optional<WaterBulletRows> rows;
    };

    // weapons: the staged weapon files to consider (content_source/weapons/<name>)
    WaterBulletStage StageWaterBullets(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots,
                                       bool hasWater, const std::vector<std::string>& weapons);
}

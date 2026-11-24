#pragma once

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace w2b::world
{
    struct GridRequest
    {
        std::filesystem::path source, destination; // WaW .lightgrid.bin -> T6 BSP/lightgrid.bin
        // static model lighting traced through the clip world, when the map has models
        struct Models
        {
            std::filesystem::path placements, clip, primaryLights;
        };
        std::optional<Models> models;
        bool compensated = true;
    };

    // Fits the static models' palettes beside the grid's into T6's 16-bit index; returns how many were shared.
    std::size_t SharePalettes(const std::vector<std::uint8_t>& grid, std::vector<std::uint8_t>& palette, util::Json& placements, std::size_t limit = 0xffff);

    // The staged grid's summary for the bridge report.
    util::Json StageGrid(const GridRequest& request);
}

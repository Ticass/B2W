#pragma once

#include "core/text.h"

#include <filesystem>
#include <optional>

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

    // The staged grid's summary for the bridge report.
    util::Json StageGrid(const GridRequest& request);
}

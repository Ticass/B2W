#pragma once

#include "core/pytext.h"
#include "world/spatial_cell.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

namespace w2b::world
{
    inline constexpr std::size_t gridPaletteSize = 168;

    struct GridEntry
    {
        std::uint16_t color;
        std::uint8_t primary;
        std::uint8_t trace;
    };

    struct LightGrid
    {
        bool hasLightRegions;
        std::uint32_t sun;
        std::array<std::uint16_t, 3> mins, maxs;
        unsigned rowAxis, colAxis;
        std::vector<std::uint16_t> rows;
        std::vector<std::uint8_t> packedRows;
        std::vector<GridEntry> entries;
        std::vector<std::uint8_t> colors;
        std::unordered_map<Cell, std::uint32_t, CellHash> points;
    };

    LightGrid ReadLightGrid(const std::filesystem::path& path);
    py::Json WriteLightGrid(const LightGrid& grid, const std::filesystem::path& path,
                            std::span<const std::uint8_t> extraColors, py::Json modelReport);
}

#include "world/light_grid.h"

#include "core/files.h"
#include "world/bsp_reader.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace w2b::world
{
    namespace
    {
        static_assert(sizeof(GridEntry) == 4);
        constexpr std::size_t headerSize = 56;

        unsigned PackedNumber(const LightGrid& grid, std::size_t offset, unsigned count)
        {
            unsigned value = 0;
            for (unsigned i = 0; i < count; ++i)
                value |= static_cast<unsigned>(grid.packedRows[offset + i]) << (i * 8);
            return value;
        }

        void ReadPoints(LightGrid& grid)
        {
            grid.points.reserve(grid.entries.size());
            const auto size = grid.packedRows.size();
            for (std::size_t row = 0; row < grid.rows.size(); ++row)
            {
                if (grid.rows[row] == 0xffff)
                    continue;
                std::size_t at = grid.rows[row] * 4;
                if (at + 12 > size)
                    throw std::runtime_error("light-grid row offset lies outside packed data");
                const auto start = PackedNumber(grid, at, 2);
                const auto columns = PackedNumber(grid, at + 2, 2);
                const auto baseZ = PackedNumber(grid, at + 4, 2);
                const auto heightZ = PackedNumber(grid, at + 6, 2);
                std::size_t entry = PackedNumber(grid, at + 8, 4);
                at += 12;
                unsigned column = 0;
                while (column < columns)
                {
                    if (at + 2 > size)
                        throw std::runtime_error("truncated light-grid run");
                    const auto span = grid.packedRows[at++];
                    const auto height = grid.packedRows[at++];
                    if (!span || column + span > columns)
                        throw std::runtime_error("invalid light-grid column run");
                    if (height)
                    {
                        const auto bytes = heightZ > 255 ? 2u : 1u;
                        if (at + bytes > size)
                            throw std::runtime_error("truncated light-grid vertical offset");
                        const auto offset = PackedNumber(grid, at, bytes);
                        at += bytes;
                        if (offset + height > heightZ || entry + span * height > grid.entries.size())
                            throw std::runtime_error("light-grid run exceeds its vertical or entry bounds");
                        for (unsigned c = 0; c < span; ++c)
                            for (unsigned z = 0; z < height; ++z)
                            {
                                Cell key{0, 0, static_cast<int>(baseZ + offset + z)};
                                key[grid.rowAxis] = static_cast<int>(grid.mins[grid.rowAxis] + row);
                                key[grid.colAxis] = static_cast<int>(start + column + c);
                                grid.points[key] = static_cast<std::uint32_t>(entry + c * height + z);
                            }
                        entry += span * height;
                    }
                    column += span;
                }
            }
        }

        template <typename T>
        void Append(std::string& bytes, const T& value)
        {
            bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
        }
    }

    LightGrid ReadLightGrid(const std::filesystem::path& path)
    {
        BspReader source(path);
        if (source.Remaining() < headerSize)
            throw std::runtime_error("truncated WaW light-grid header");
        const auto size = source.Remaining();
        const auto magic = source.One<std::array<char, 8>>();
        const auto version = source.One<std::uint32_t>();
        const auto regions = source.One<std::uint32_t>();
        if (std::string_view(magic.data(), magic.size()) != "W2BLGRD1" || version != 1 || regions > 1)
            throw std::runtime_error("unsupported WaW light-grid header");
        LightGrid grid{};
        grid.hasLightRegions = regions != 0;
        grid.sun = source.One<std::uint32_t>();
        grid.mins = source.One<std::array<std::uint16_t, 3>>();
        grid.maxs = source.One<std::array<std::uint16_t, 3>>();
        const auto [row, col, rows, raw, entries, colors] = source.One<std::array<std::uint32_t, 6>>();
        if (row > 1 || col > 1 || row == col || grid.maxs[row] < grid.mins[row])
            throw std::runtime_error("invalid WaW light-grid axes/bounds");
        if (rows && rows != 1u + grid.maxs[row] - grid.mins[row])
            throw std::runtime_error("light-grid row count disagrees with bounds");
        const auto expected = headerSize + 2ull * rows + raw + 4ull * entries + gridPaletteSize * std::uint64_t{colors};
        if (size != expected)
            throw std::runtime_error("WaW light-grid length " + std::to_string(size) + " != " + std::to_string(expected));
        grid.rowAxis = row;
        grid.colAxis = col;
        grid.rows = source.Array<std::uint16_t>(rows);
        grid.packedRows = source.Array<std::uint8_t>(raw);
        grid.entries = source.Array<GridEntry>(entries);
        grid.colors = source.Array<std::uint8_t>(static_cast<std::size_t>(colors) * gridPaletteSize);
        if (std::ranges::any_of(grid.entries, [colors](const GridEntry& entry) { return entry.color >= colors; }))
            throw std::runtime_error("WaW light-grid entry references an absent palette");
        ReadPoints(grid);
        return grid;
    }

    util::Json WriteLightGrid(const LightGrid& grid, const files::fs::path& path,
                            std::span<const std::uint8_t> extraColors, util::Json modelReport)
    {
        const auto count = (grid.colors.size() + extraColors.size()) / gridPaletteSize;
        if (!modelReport.is_null() && count > 0xffff)
            throw std::runtime_error("static model lighting exceeds the T6 palette index range");
        std::string output = "W2BT6LG1";
        output.reserve(headerSize + grid.rows.size() * 2 + grid.packedRows.size() + grid.entries.size() * 4 +
                       grid.colors.size() + extraColors.size());
        Append(output, std::array<std::uint32_t, 3>{1, 0, grid.sun});
        Append(output, grid.mins);
        Append(output, grid.maxs);
        Append(output, std::array<std::uint32_t, 6>{grid.rowAxis, grid.colAxis, static_cast<std::uint32_t>(grid.rows.size()),
               static_cast<std::uint32_t>(grid.packedRows.size()), static_cast<std::uint32_t>(grid.entries.size()),
               static_cast<std::uint32_t>(count)});
        for (auto row : grid.rows)
            Append(output, row);
        if (!grid.packedRows.empty())
            output.append(reinterpret_cast<const char*>(grid.packedRows.data()), grid.packedRows.size());
        std::size_t tracePoints = 0;
        for (const auto& entry : grid.entries)
        {
            Append(output, GridEntry{entry.color, entry.primary, static_cast<std::uint8_t>(entry.primary ? 255 : 0)});
            tracePoints += entry.trace != 0;
        }
        const auto scale = std::sqrt(8.0);
        for (const auto& colors : {std::span(grid.colors), extraColors})
            for (const auto color : colors)
                output += static_cast<char>(std::nearbyint(color / scale));
        files::WriteBytes(path, output);
        return {{"bounds", {grid.mins, grid.maxs}}, {"axes", {grid.rowAxis, grid.colAxis}}, {"rows", grid.rows.size()},
                {"packed_row_bytes", grid.packedRows.size()}, {"entries", grid.entries.size()},
                {"colors", grid.colors.size() / gridPaletteSize}, {"sun_index", grid.sun}, {"status", "runtime_T6_grid"},
                {"palette_encoding", "WaW_gamma2_to_T6_linear32"}, {"static_model_lighting", std::move(modelReport)},
                {"source_trace_mask_points", tracePoints},
                {"corner_obstruction_checks", "T4_runtime_traces_not_representable_in_T6_visibility"}};
    }
}

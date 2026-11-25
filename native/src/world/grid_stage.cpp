#include "world/grid_stage.h"

#include "core/files.h"
#include "core/parallel.h"
#include "world/grid_models.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <string>

namespace w2b::world
{
    std::size_t SharePalettes(const std::vector<std::uint8_t>& grid, std::vector<std::uint8_t>& palette, util::Json& placements, std::size_t limit)
    {
        // T6 indexes every palette (the grid's and one per static model) with 16 bits. A map's own grid can fill
        // nearly all of it (Purple Dimension): equal palettes become one, then the models that change least take the
        // closest grid palette.
        const auto gridCount = grid.size() / gridPaletteSize, count = palette.size() / gridPaletteSize;
        if (gridCount + count <= limit)
            return 0;
        const auto at = [](const std::vector<std::uint8_t>& colors, std::size_t i) { return colors.data() + i * gridPaletteSize; };
        std::map<std::string, std::size_t> first;
        std::vector<std::size_t> same(count), unique;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto [entry, added] = first.try_emplace(std::string(reinterpret_cast<const char*>(at(palette, i)), gridPaletteSize), i);
            same[i] = entry->second;
            if (added)
                unique.push_back(i);
        }
        struct Nearest
        {
            std::uint64_t error = std::numeric_limits<std::uint64_t>::max();
            std::size_t color = 0;
        };
        std::vector<Nearest> nearest(count);
        const auto room = limit > gridCount ? limit - gridCount : 0;
        if (unique.size() > room)
        {
            parallel::For(unique.size(), [&](std::size_t u) {
                const auto* wanted = at(palette, unique[u]);
                auto& best = nearest[unique[u]];
                for (std::size_t g = 0; g < gridCount; ++g)
                {
                    const auto* color = at(grid, g);
                    std::uint64_t error = 0;
                    for (std::size_t b = 0; b < gridPaletteSize && error < best.error; ++b)
                    {
                        const auto d = static_cast<int>(wanted[b]) - color[b];
                        error += static_cast<std::uint64_t>(d * d);
                    }
                    if (error < best.error)
                        best = {error, g};
                }
            });
            std::ranges::stable_sort(unique, [&](std::size_t a, std::size_t b) { return nearest[a].error > nearest[b].error; });
        }
        std::vector<std::uint8_t> kept;
        std::vector<std::size_t> index(count);
        std::size_t shared = count - unique.size();
        for (std::size_t u = 0; u < unique.size(); ++u)
            if (u < room)
            {
                index[unique[u]] = gridCount + kept.size() / gridPaletteSize;
                kept.insert(kept.end(), at(palette, unique[u]), at(palette, unique[u]) + gridPaletteSize);
            }
            else
            {
                index[unique[u]] = nearest[unique[u]].color;
                ++shared;
            }
        for (auto& model : placements.at("models"))
            if (const auto color = model.find("colorsIndex"); color != model.end())
                if (const auto i = color->get<std::size_t>(); i >= gridCount)
                    *color = index.at(same.at(i - gridCount));
        palette = std::move(kept);
        return shared;
    }

    util::Json StageGrid(const GridRequest& request)
    {
        using util::Json;
        auto grid = ReadLightGrid(request.source);
        if (!request.models)
            return WriteLightGrid(grid, request.destination, {}, {});

        const auto collision = ReadCollisionWorld(request.models->clip);
        const WorldTracer tracer(collision, 0x2001, request.compensated);
        auto placements = Json::parse(files::ReadText(request.models->placements));
        const auto lights = Json::parse(files::ReadText(request.models->primaryLights));
        auto samples = SampleModelLighting(grid, placements, tracer, lights.at("lights"), request.compensated);
        if (const auto shared = SharePalettes(grid.colors, samples.palette, placements))
            samples.summary["shared_palettes"] = shared;
        files::WriteText(request.models->placements, util::JsonDumps(placements, 1) + "\n");
        const auto filled = FillHiddenPoints(grid, tracer, (grid.colors.size() + samples.palette.size()) / gridPaletteSize, samples.summary);
        samples.palette.insert(samples.palette.end(), filled.begin(), filled.end());
        return WriteLightGrid(grid, request.destination, samples.palette, std::move(samples.summary));
    }
}

#include "world/grid_models.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        constexpr Vec3 cellSize{32.0, 32.0, 64.0};
        constexpr double gridOffset = 131072.0;
        constexpr double cornerMinWeight = 0.001;

        struct Corner
        {
            Vec3 position;
            double weight;
            const GridEntry* entry;
            std::optional<bool> visible;

            bool Visible(const WorldTracer& tracer, const Vec3& origin)
            {
                if (!visible)
                {
                    Vec3 end;
                    for (std::size_t axis = 0; axis < end.size(); ++axis)
                        end[axis] = position[axis] + 0.01 * (origin[axis] - position[axis]);
                    visible = tracer.Visible(origin, end);
                }
                return *visible;
            }
        };

        struct ModelCell
        {
            Cell first;
            Vec3 fractions;

            explicit ModelCell(const Vec3& origin)
            {
                for (std::size_t axis = 0; axis < first.size(); ++axis)
                {
                    const auto value = (origin[axis] + gridOffset) / cellSize[axis];
                    first[axis] = FloorCell(value);
                    fractions[axis] = value - first[axis];
                }
            }
        };

        std::array<Corner, 8> Corners(const LightGrid& grid, const ModelCell& cell)
        {
            std::array<Corner, 8> corners;
            for (unsigned bits = 0; bits < corners.size(); ++bits)
            {
                Cell step{};
                step[grid.rowAxis] = (bits & 4) != 0;
                step[grid.colAxis] = (bits & 2) != 0;
                step[2] = (bits & 1) != 0;
                Cell key;
                Vec3 position;
                double weight = 1.0;
                for (std::size_t axis = 0; axis < key.size(); ++axis)
                {
                    key[axis] = cell.first[axis] + step[axis];
                    weight *= step[axis] ? cell.fractions[axis] : 1.0 - cell.fractions[axis];
                    position[axis] = key[axis] * cellSize[axis] - gridOffset;
                }
                const auto found = grid.points.find(key);
                corners[bits] = {position, weight, found == grid.points.end() ? nullptr : &grid.entries[found->second], {}};
            }
            return corners;
        }

        int DominantLight(std::array<Corner, 8>& corners, const WorldTracer& tracer,
                          const Vec3& origin, std::vector<Corner*>& kept)
        {
            bool found = false;
            int light = 0;
            double best = 0;
            for (std::size_t i = 0; i < corners.size(); ++i)
            {
                auto& corner = corners[i];
                if (!corner.entry || corner.weight < cornerMinWeight)
                    continue;
                const auto primary = corner.entry->primary;
                const bool blocked = (corner.entry->trace & (1u << i)) && !corner.Visible(tracer, origin);
                if (blocked && found)
                    continue;
                if (!blocked && !found)
                {
                    found = true;
                    best = corner.weight;
                    light = primary;
                    kept.clear();
                    kept.push_back(&corner);
                    continue;
                }
                kept.push_back(&corner);
                if (light == 0 || (primary && (light == 255 || (primary != 255 && corner.weight > best))))
                {
                    best = corner.weight;
                    light = primary;
                }
            }
            return light;
        }

        bool InLightVolume(const util::Json& light, const Vec3& position, bool compensated)
        {
            const auto type = light.at("type").get<int>();
            if (type < 2)
                return type == 1;
            const auto delta = Subtract(light.at("origin").get<Vec3>(), position);
            const auto distance = SumDot(delta, delta, compensated);
            const auto radius = light.at("radius").get<double>();
            if (distance >= radius * radius)
                return false;
            if (type == 3)
                return true;
            const auto outer = light.at("cosHalfFovOuter").get<double>();
            const auto& limit = light.at("rotationLimit");
            const auto rotation = limit.get<double>();
            if (!(-outer < rotation))
                return true;
            if (rotation != 1.0)
            {
                const auto id = light.value("index", util::Json{});
                throw std::runtime_error("primary light " + (id.is_null() ? "None" : id.dump()) +
                    ": rotationLimit " + util::JsonDumps(limit, -1) +
                    " needs WaW's expanded cone (sub_595370), not ported");
            }
            const auto along = SumDot(light.at("dir").get<Vec3>(), delta, compensated);
            return along > 0 && outer * outer * distance <= along * along;
        }

        using Columns = std::map<std::pair<int, int>, int>;

        bool BelowGrid(const LightGrid& grid, const Columns& columns, const ModelCell& cell)
        {
            const auto row = cell.first[grid.rowAxis];
            const auto col = cell.first[grid.colAxis];
            for (const auto lookup : {row, row + 1})
            {
                const auto found = columns.find({lookup, col});
                if (found != columns.end() && cell.first[2] < found->second)
                    return true;
            }
            return false;
        }

        struct Sample
        {
            std::size_t color;
            int visibility, light;
        };

        Sample BlendModel(const LightGrid& grid, const util::Json& model, const Columns& columns,
                          const WorldTracer& tracer, const util::Json& lights, bool compensated,
                          std::vector<std::uint8_t>& palette, std::size_t& outside, std::size_t& unreached)
        {
            const auto origin = model.at("lightingOrigin").get<Vec3>();
            const ModelCell cell(origin);
            auto corners = Corners(grid, cell);
            outside += std::ranges::none_of(corners, [](const Corner& corner) { return corner.entry != nullptr; });
            std::vector<Corner*> kept;
            kept.reserve(corners.size());
            const auto dominant = DominantLight(corners, tracer, origin, kept);
            const int sun = static_cast<int>(grid.sun);
            const auto light = dominant == 255 ? sun :
                grid.hasLightRegions && dominant != sun ? model.value("primaryLightIndex", 0) : dominant;
            double lit = 0, against = 0, total = 0;
            std::array<double, gridPaletteSize> blend{};
            for (const auto corner : kept)
            {
                if (!corner->Visible(tracer, origin))
                    continue;
                const auto primary = corner->entry->primary;
                const auto weight = corner->weight;
                if (primary == light)
                    lit += weight;
                else if ((primary == 0 || (primary == 255 && light != 0)) &&
                         InLightVolume(lights.at(light), corner->position, compensated))
                    against += weight;
                total += weight;
                const auto offset = corner->entry->color * gridPaletteSize;
                for (std::size_t i = 0; i < blend.size(); ++i)
                    blend[i] += grid.colors[offset + i] * weight;
            }
            if (total == 0)
            {
                ++unreached;
                return {BelowGrid(grid, columns, cell) ? 0u : 1u, 255, sun};
            }
            const double visible = !light ? 0.0 : against == 0 ? (lit != 0 ? 1.0 : 0.0) : lit / (lit + against);
            for (const auto value : blend)
                palette.push_back(static_cast<std::uint8_t>(std::min(255.0, std::nearbyint(value / total))));
            return {(grid.colors.size() + palette.size()) / gridPaletteSize - 1,
                    static_cast<int>(visible * 255.0 + 0.5), light};
        }
    }

    ModelGridSamples SampleModelLighting(const LightGrid& grid, util::Json& placements, const WorldTracer& tracer,
                                        const util::Json& lights, bool compensated)
    {
        Columns columns;
        for (const auto& [point, _] : grid.points)
        {
            const auto [column, added] = columns.try_emplace({point[grid.rowAxis], point[grid.colAxis]}, point[2]);
            if (!added)
                column->second = std::min(column->second, point[2]);
        }
        std::vector<std::uint8_t> palette;
        std::size_t count = 0, unreached = 0, outside = 0;
        for (auto& model : placements.at("models"))
        {
            if (!model.contains("lightingOrigin"))
                continue;
            ++count;
            const auto sample = BlendModel(grid, model, columns, tracer, lights, compensated, palette, outside, unreached);
            const auto compiled = model.value("primaryLightIndex", util::Json{});
            if (compiled != sample.light)
                model["compiledPrimaryLightIndex"] = compiled;
            model["colorsIndex"] = sample.color;
            model["visibility"] = sample.visibility;
            model["primaryLightIndex"] = sample.light;
        }
        return {std::move(palette), {{"static_models", count}, {"no_corner_reached", unreached}, {"outside_grid", outside}}};
    }
}

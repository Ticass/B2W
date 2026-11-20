#include "mapents/entities.h"

#include "core/files.h"
#include "world/collision_world.h"

#include <algorithm>
#include <locale>
#include <set>
#include <sstream>

namespace w2b::mapents
{
    long long BrushIndex(std::string_view model)
    {
        long long index{};
        bool overflow{};
        if (!util::ParseInt(model.substr(1), index, overflow) || overflow)
            throw std::invalid_argument("invalid entity brush model: " + std::string(model));
        return index;
    }

    namespace
    {
        std::array<double, 3> ParseOrigin(std::string_view text, std::string_view authored)
        {
            std::istringstream input{std::string(text)};
            input.imbue(std::locale::classic());
            std::array<double, 3> result{};
            std::string extra;
            if (!(input >> result[0] >> result[1] >> result[2]) || input >> extra)
                throw std::invalid_argument("invalid entity origin: " + std::string(authored));
            return result;
        }
    }

    std::array<double, 3> Origin(const MapEntity& entity)
    {
        return ParseOrigin(Field(entity, "origin", "0 0 0"), Field(entity, "origin"));
    }

    std::array<double, 3> Origin(const Json& entity)
    {
        return ParseOrigin(Field(entity, "origin", "0 0 0"), Field(entity, "origin"));
    }

    namespace
    {
        void MarkChasers(MapEntities& entities, Json& summary)
        {
            if (std::ranges::any_of(entities, [](const MapEntity& ent) { return Field(ent, "targetname") == "exterior_goal"; }))
                return;
            unsigned count = 0;
            for (auto& ent : entities)
            {
                const auto role = Field(ent, "script_noteworthy");
                if (Field(ent, "classname") == "script_struct" && (role == "spawn_location" || role == "riser_location") &&
                    Field(ent, "script_string").empty())
                {
                    ent.Set("script_string", "find_flesh");
                    ++count;
                }
            }
            if (count)
                summary["renamed"]["no exterior_goal barricades: spawn -> find_flesh"] = count;
        }

        bool InVolume(const std::array<double, 3>& point, const MapEntity& entity, const world::CollisionWorld& clip)
        {
            const auto model = Field(entity, "model");
            if (!model.starts_with('*'))
                return false;
            auto index = BrushIndex(model);
            const auto size = static_cast<std::int64_t>(clip.models.size());
            if (index >= size)
                return false;
            if (index < 0)
                index += size;
            const auto origin = Origin(entity);
            const std::array local{point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
            for (const auto brush : clip.models.at(static_cast<std::size_t>(index)).brushes)
                if (std::ranges::all_of(clip.brushes.at(brush).planes, [&](const world::BrushPlane& plane) {
                        return plane.normal[0] * local[0] + plane.normal[1] * local[1] + plane.normal[2] * local[2] <= plane.distance + 0.1;
                    }))
                    return true;
            return false;
        }

        void AddInitialSpawns(MapEntities& entities, Json& spawns, Json& summary, const world::CollisionWorld* clip,
                              const std::optional<std::vector<std::string>>& startZones)
        {
            constexpr std::size_t CoopPlayers = 4;
            std::vector<std::size_t> starts, volumes;
            using Names = std::set<std::string, std::less<>>;
            const auto names = startZones ? Names(startZones->begin(), startZones->end()) : Names{};
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entity = entities.at(i);
                const auto name = Field(entity, "targetname");
                if (name == "initial_spawn_points")
                    starts.push_back(i);
                if ((Field(entity, "classname") == "info_volume" && names.contains(name)) ||
                    Field(entity, "script_noteworthy") == "life_brush")
                    volumes.push_back(i);
            }
            if (starts.empty())
                return;
            const auto count = std::min(CoopPlayers, starts.size());
            std::size_t outside = 0;
            auto ffa = Json::array();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& entity = entities.at(starts[i]);
                if (clip && !volumes.empty())
                {
                    auto point = Origin(entity);
                    point[2] += 32;
                    if (std::ranges::none_of(volumes, [&](auto volume) { return InVolume(point, entities.at(volume), *clip); }))
                        ++outside;
                }
                const auto origin = std::string(Field(entity, "origin", "0 0 0")), angles = std::string(Field(entity, "angles", "0 0 0"));
                if (i == 0)
                    ffa.push_back({{"origin", origin}, {"angles", angles}});
                MapEntity spawn{{"classname", "script_struct"}, {"targetname", "waw2bo2_initial_spawn"},
                                {"script_noteworthy", "initial_spawn"}, {"script_string", "zclassic_classic_spawn"},
                                {"origin", origin}, {"angles", angles}};
                entities.push_back(std::move(spawn));
            }
            summary["spawn_points"] = count;
            summary["spawn_points_unused_by_waw_coop_placement"] = starts.size() - count;
            summary["spawn_points_outside_start_zones"] = outside;
            spawns["FFA"] = std::move(ffa);
        }
    }

    MapEntityResult Write(Converted converted, const Path& bsp, const world::CollisionWorld* clip, std::optional<std::vector<std::string>> startZones)
    {
        auto& entities = converted.entities;
        auto& spawns = converted.spawns;
        auto& summary = converted.summary;
        if (startZones && startZones->empty() && clip && std::filesystem::exists(bsp / "submodels.json"))
            startZones = SynthesizeZones(entities, *clip, bsp, summary);
        MarkChasers(entities, summary);
        AddInitialSpawns(entities, spawns, summary, clip, startZones);
        std::set<std::string> models;
        for (const auto& entity : entities)
        {
            const auto model = Field(entity, "model");
            if (!model.empty() && !model.starts_with('*') && Field(entity, "classname") == "script_model")
                models.emplace(model);
        }
        WriteMapEntities(bsp / "entities.json", entities);
        files::WriteText(bsp / "spawns.json", util::JsonDumps(spawns, 1) + "\n");
        return {std::move(summary), {models.begin(), models.end()}, std::nullopt};
    }
}

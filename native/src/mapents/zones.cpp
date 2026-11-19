#include "mapents/entities.h"

#include "core/files.h"
#include "world/collision_world.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace w2b::mapents
{
    namespace
    {
        using Vec3 = std::array<double, 3>;
        using Names = std::set<std::string, std::less<>>;
        using Flags = std::array<std::uint32_t, 2>;
        constexpr std::string_view InitSpawners = "zombie_spawner_init";
        constexpr std::string_view FirstZone = "waw2bo2_zone";

        Flags VolumeFlags(const MapEntities& entities, const Json& submodels)
        {
            Names models;
            for (const auto& entity : entities)
            {
                const auto model = Field(entity, "model"), type = Field(entity, "classname");
                if (model.starts_with('*') && (type.starts_with("info_volume") || type.starts_with("trigger_")))
                    models.emplace(model);
            }
            for (const auto& model : models)
            {
                auto index = BrushIndex(model);
                if (index == std::numeric_limits<long long>::min())
                    throw std::invalid_argument("invalid trigger brush model: " + model);
                --index;
                const auto size = static_cast<std::int64_t>(submodels.size());
                if (index >= size)
                    continue;
                if (index < 0)
                    index += size;
                const auto& brushes = submodels.at(static_cast<std::size_t>(index)).at("brushes");
                if (!brushes.empty())
                    return {brushes.at(0).at("contents").get<std::uint32_t>(), brushes.at(0).at("axial").at(0).at(1).get<std::uint32_t>()};
            }
            return {134217729, 262272};
        }

        Json BoxBrush(const Vec3& mins, const Vec3& maxs, const Flags& flags)
        {
            auto corners = Json::array();
            for (const auto x : {mins[0], maxs[0]})
                for (const auto y : {mins[1], maxs[1]})
                    for (const auto z : {mins[2], maxs[2]})
                        corners.push_back({x, y, z});
            auto axial = Json::array();
            for (int side = 0; side < 6; ++side)
                axial.push_back(flags);
            return {{"mins", mins}, {"maxs", maxs}, {"contents", flags[0]}, {"axial", std::move(axial)},
                    {"sides", Json::array()}, {"verts", std::move(corners)}};
        }

        Vec3 Offset(const world::ClipPoint& point, double delta)
        {
            return {point[0] + delta, point[1] + delta, point[2] + delta};
        }

        bool IsDoor(const MapEntity& entity)
        {
            const auto name = Field(entity, "targetname");
            return name == "zombie_door" || name == "zombie_debris";
        }

        bool ConnectDoors(MapEntities& entities, std::string_view group, const std::string& zone)
        {
            std::set<std::size_t> doors;
            std::multimap<std::string_view, std::size_t> referenced;
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entity = entities.at(i);
                if (IsDoor(entity))
                    referenced.emplace(Field(entity, "target"), i);
            }
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entity = entities.at(i);
                if (Field(entity, "classname") == ZombieActor || (Field(entity, "target") != group && Field(entity, "script_string") != group))
                    continue;
                if (IsDoor(entity))
                    doors.insert(i);
                if (const auto name = Field(entity, "targetname"); !name.empty())
                {
                    const auto [begin, end] = referenced.equal_range(name);
                    for (auto link = begin; link != end; ++link)
                        doors.insert(link->second);
                }
            }
            referenced.clear();
            for (const auto index : doors)
            {
                auto& door = entities.at(index);
                const auto prior = std::string(Field(door, "script_flag"));
                const auto flag = zone + "_open";
                door.Set("script_flag", prior.empty() ? flag : prior + "," + flag);
            }
            return !doors.empty();
        }
    }

    Json SynthesizeZones(MapEntities& entities, const world::CollisionWorld& clip, const Path& bsp, Json& summary)
    {
        std::map<std::string, std::vector<std::size_t>, std::less<>> spawners;
        std::set<std::pair<std::string, std::string>> structs;
        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            const auto& entity = entities.at(i);
            const auto type = Field(entity, "classname"), name = Field(entity, "targetname");
            if (type == ZombieActor && !name.empty())
                spawners[std::string(name)].push_back(i);
            if (type == "script_struct")
                structs.emplace(name, Field(entity, "origin"));
        }
        if (spawners.empty())
            throw std::invalid_argument("map has no zone graph and no named zombie spawners");
        std::vector<std::string> groups;
        for (const auto& [name, _] : spawners)
            groups.push_back(name);
        if (const auto init = std::ranges::find(groups, InitSpawners); init != groups.end())
            std::rotate(groups.begin(), init, init + 1);

        const auto path = bsp / "submodels.json";
        auto submodels = Json::parse(files::ReadText(path)).at("submodels");
        const auto flags = VolumeFlags(entities, submodels);
        const auto& world = clip.models.at(0);
        const auto mins = Offset(world.mins, -512), maxs = Offset(world.maxs, 512);
        const auto cornerMin = Offset(world.mins, 0), cornerMax = Offset(world.mins, 16);
        auto initial = Json::array(), adjacency = Json::array(), locked = Json::array();
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            const auto& group = groups[i];
            auto zone = std::string(FirstZone);
            if (i != 0)
                zone += "_" + std::to_string(i);
            const bool unlocked = i == 0 || group == InitSpawners;
            if (unlocked)
                initial.push_back(zone);
            else if (ConnectDoors(entities, group, zone))
                adjacency.push_back("\"" + std::string(FirstZone) + "\", \"" + zone + "\", \"" + zone + "_open\"");
            else
            {
                locked.push_back(group);
                continue;
            }
            const auto& lo = unlocked ? mins : cornerMin;
            const auto& hi = unlocked ? maxs : cornerMax;
            submodels.push_back({{"mins", lo}, {"maxs", hi}, {"brushes", Json::array({BoxBrush(lo, hi, flags)})}});
            entities.push_back({{"classname", "info_volume"}, {"targetname", zone}, {"target", group}, {"origin", "0 0 0"},
                                {"model", "*" + std::to_string(submodels.size())}, {"script_noteworthy", "player_volume"}});
            for (const auto index : spawners.at(group))
            {
                if (!structs.contains({group, std::string(Field(entities.at(index), "origin"))}))
                {
                    auto marker = SpawnMarker(entities.at(index), group);
                    entities.push_back(std::move(marker));
                }
            }
        }
        files::WriteText(path, py::JsonDumps({{"submodels", std::move(submodels)}}, -1) + "\n");
        summary["synthesized_zones"] = {{"initial", initial}, {"adjacency", std::move(adjacency)}, {"groups", groups},
                                        {"never_unlocked", std::move(locked)}};
        return initial;
    }
}

#include "mapents/entities.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>

namespace w2b::mapents
{
    std::string_view Field(const Json& entity, std::string_view key, std::string_view fallback)
    {
        const auto field = entity.find(key);
        return field == entity.end() ? fallback : std::string_view(field->get_ref<const std::string&>());
    }

    void Increment(Json& counters, std::string_view key)
    {
        const auto count = counters.value(key, 0u);
        counters[key] = count + 1;
    }

    namespace
    {
        using Names = std::set<std::string, std::less<>>;

        MapEntity Placement(const MapEntity& entity)
        {
            return {{"origin", std::string(Field(entity, "origin", "0 0 0"))}, {"angles", std::string(Field(entity, "angles", "0 0 0"))}};
        }

        MapEntity Worldspawn(const MapEntities& entities, std::string_view project)
        {
            static constexpr std::array<std::string_view, 8> keys{
                "classname", "skyboxmodel", "sundirection", "suncolor", "sunlight", "_color", "ambient", "diffusefraction"};
            MapEntity result;
            const auto world = std::ranges::find_if(entities, [](const MapEntity& e) { return Field(e, "classname") == "worldspawn"; });
            if (world != entities.end())
                for (const auto& [key, value] : world->fields)
                    if (std::ranges::find(keys, key) != keys.end())
                        result.Set(key, value);
            result.Set("classname", "worldspawn");
            result.Set("skyboxmodel", "skybox_" + std::string(project));
            return result;
        }

        void StructDefaults(MapEntity& entity)
        {
            if (!entity.Has("origin"))
                entity.Set("origin", "0 0 0");
            if (entity.Has("angles"))
                return;
            const auto angle = std::string(Field(entity, "angle", "0"));
            if (angle == "-1")
                entity.Set("angles", "-90 0 0");
            else if (angle == "-2")
                entity.Set("angles", "90 0 0");
            else
                entity.Set("angles", "0 " + angle + " 0");
        }

        MapEntity ZombieMarker(const MapEntity& source, std::string_view target, std::string_view noteworthy)
        {
            return {{"classname", "script_struct"}, {"targetname", std::string(target)}, {"origin", std::string(Field(source, "origin", "0 0 0"))},
                    {"angles", std::string(Field(source, "angles", "0 0 0"))}, {"script_noteworthy", std::string(noteworthy)}};
        }

        MapEntity Barrier(const MapEntity& goal, std::string_view target)
        {
            MapEntity entity{{"classname", "zbarrier_zmcore_BasicWoodBarrier"}, {"type", "zmcore_BasicWoodBarrier"},
                             {"targetname", std::string(target)}, {"origin", std::string(Field(goal, "origin", "0 0 0"))},
                             {"angles", std::string(Field(goal, "angles", "0 0 0"))}, {"zbarriernumboards", "6"}};
            for (int i = 1; i <= 6; ++i)
                entity.Set("zbarrierboardmodel" + std::to_string(i), "p6_anim_zm_barricade_board_0" + std::to_string(i));
            for (int i = 1; i <= 6; ++i)
                entity.Set("zbarrierboardanim" + std::to_string(i), "o_zombie_board_" + std::to_string(i) + "_repair");
            for (int i = 1; i <= 6; ++i)
                entity.Set("zbarriertearanim" + std::to_string(i), "o_zombie_board_" + std::to_string(i) + "_pull");
            return entity;
        }

        class Converter
        {
        public:
            Converter(const Renames& animscripts, std::string_view respawnExclusion)
                : animscripts_(animscripts), respawnExclusion_(respawnExclusion) {}

            Converted Run(MapEntities& entities, std::string_view project, const Names& zones)
            {
                CollectTargets(entities, zones);
                output_.push_back(Worldspawn(entities, project));
                MapEntities starts, fallback;
                for (auto& entity : entities)
                {
                    if (Field(entity, "targetname") == "initial_spawn_points")
                        starts.push_back(Placement(entity));
                    if (Field(entity, "classname") == "info_player_start")
                        fallback.push_back(Placement(entity));
                    Append(std::move(entity));
                }
                NormalizeWallBuys(output_, renamed_);
                if (starts.empty())
                    starts = std::move(fallback);
                auto ffa = Json::array();
                if (!starts.empty())
                {
                    ffa.push_back(ToJson(starts.front()));
                    if (!intermission_)
                    {
                        MapEntity entity{{"classname", "mp_global_intermission"}};
                        for (const auto& [key, value] : starts.front().fields)
                            entity.Set(key, value);
                        output_.push_back(std::move(entity));
                    }
                }
                Json report{{"entities", output_.size()}, {"dropped", std::move(dropped_)}, {"renamed", std::move(renamed_)},
                            {"spawn_points", starts.size()}};
                Json spawns{{"attackers", Json::array()}, {"defenders", Json::array()}, {"FFA", std::move(ffa)}};
                return {std::move(output_), std::move(spawns), std::move(report)};
            }

        private:
            void CollectTargets(MapEntities& entities, const Names& zones)
            {
                // Normalize zone volumes first, so actor and rise markers see the same targets.
                for (auto& entity : entities)
                {
                    const auto name = std::string(Field(entity, "targetname"));
                    const auto type = std::string(Field(entity, "classname"));
                    if (type != "info_volume" && entity.Has("targetname") && zones.contains(name) && Field(entity, "model").starts_with('*'))
                    {
                        Increment(renamed_, "zone volume " + type + " -> info_volume");
                        entity.Set("classname", "info_volume");
                    }
                    if (!name.empty())
                        barriers_[name] |= Field(entity, "classname").starts_with("zbarrier_");
                    if (Field(entity, "classname") == "info_volume")
                        if (const auto target = Field(entity, "target"); !target.empty())
                            volumeTargets_.emplace(target);
                }
            }

            bool PrepareStruct(MapEntity& entity, std::string_view targetname)
            {
                StructDefaults(entity);
                if (targetname == "exterior_goal")
                {
                    const auto target = Field(entity, "target");
                    if (target.empty())
                    {
                        Increment(dropped_, "exterior_goal without target (a window marker with no window)");
                        return false;
                    }
                    if (const auto group = barriers_.find(target); group != barriers_.end() && !group->second)
                    {
                        output_.push_back(Barrier(entity, target));
                        Increment(renamed_, "WaW brush barricades -> T6 zbarriers");
                    }
                }
                if (targetname == "player_respawn_point" && Field(entity, "script_noteworthy").empty() && Field(entity, "script_string").empty())
                {
                    entity.Set("script_string", std::string(respawnExclusion_));
                    Increment(renamed_, "WaW team respawn anchor -> source callback (native gametype exclusion)");
                }
                return true;
            }

            std::optional<MapEntity> PlayerVolume(MapEntity& entity)
            {
                const auto tag = std::string(Field(entity, "script_noteworthy"));
                if (tag == "player_volume")
                    return std::nullopt;
                if (tag.empty())
                {
                    entity.Set("script_noteworthy", "player_volume");
                    Increment(renamed_, "info_volume without script_noteworthy -> player_volume");
                    return std::nullopt;
                }
                auto twin = entity;
                twin.Set("script_noteworthy", "player_volume");
                Increment(renamed_, "info_volume script_noteworthy " + tag + " + player_volume twin");
                return twin;
            }

            bool ConvertActor(MapEntity& entity, const std::string& type, const std::string& name)
            {
                if (type.find("zombie") == type.npos && Field(entity, "script_noteworthy") != "zombie_spawner")
                {
                    Increment(dropped_, type + " (WaW human AI; T6 zombies has no aitype for it)");
                    return false;
                }
                Increment(renamed_, type + " -> " + std::string(ZombieActor));
                entity.Set("classname", std::string(ZombieActor));
                entity.Set("waw_actor_type", type.substr(6));
                entity.Erase("model");
                if (Field(entity, "script_forcespawn") != "1")
                {
                    entity.Set("script_forcespawn", "1");
                    Increment(renamed_, "WaW DoSpawn -> T6 spawnactor spawner flag");
                }
                if (Field(entity, "script_noteworthy") != "zombie_spawner" || name.empty())
                    return true;
                const auto mode = Field(entity, "script_string");
                if (mode == "riser")
                {
                    Increment(renamed_, "WaW riser spawner (rises at its zone's _rise structs)");
                    return true;
                }
                if (mode == "zombie_chaser")
                    Increment(renamed_, "WaW zombie_chaser spawner -> find_flesh spawn_location");
                output_.push_back(SpawnMarker(entity, name));
                Increment(renamed_, "WaW zombie spawner -> spawn_location struct");
                return true;
            }

            void AppendZoneMarker(const MapEntity& source, std::string_view targetname)
            {
                const auto add = [&](std::string_view suffix, std::string_view role, std::string_view counter) {
                    if (!targetname.ends_with(suffix))
                        return;
                    const auto group = targetname.substr(0, targetname.size() - suffix.size());
                    if (!volumeTargets_.contains(group))
                        return;
                    auto marker = ZombieMarker(source, group, role);
                    if (role == "riser_location" && Field(source, "script_noteworthy") == "find_flesh")
                        marker.Set("script_string", "find_flesh");
                    output_.push_back(std::move(marker));
                    Increment(renamed_, counter);
                };
                add("_rise", "riser_location", "WaW rise struct -> riser_location struct");
                add("_dog", "dog_location", "WaW dog struct -> dog_location struct");
            }

            void Append(MapEntity entity)
            {
                const auto type = std::string(Field(entity, "classname"));
                if (type == "worldspawn")
                    return;
                if (type.starts_with("node_"))
                {
                    if (entity.Has("animscript"))
                        if (const auto script = animscripts_.find(Field(entity, "animscript")); script != animscripts_.end())
                            entity.Set("animscript", script->second);
                    output_.push_back(std::move(entity));
                    return;
                }
                const auto name = std::string(Field(entity, "targetname"));
                if (type == "script_struct" && !PrepareStruct(entity, name))
                    return;
                auto twin = type == "info_volume" ? PlayerVolume(entity) : std::nullopt;
                if (type == "trigger_multiple" && name == "playable_area" && !entity.Has("script_noteworthy"))
                {
                    entity.Set("script_noteworthy", "life_brush");
                    Increment(renamed_, "trigger_multiple playable_area -> life_brush");
                }
                if (type.starts_with("actor_") && !ConvertActor(entity, type, name))
                    return;
                if (type == "script_struct")
                    AppendZoneMarker(entity, name);
                intermission_ |= type == "mp_global_intermission";
                output_.push_back(std::move(entity));
                if (twin)
                    output_.push_back(std::move(*twin));
            }

            const Renames& animscripts_;
            std::string_view respawnExclusion_;
            std::map<std::string, bool, std::less<>> barriers_;
            Names volumeTargets_;
            bool intermission_ = false;
            MapEntities output_;
            Json dropped_ = Json::object();
            Json renamed_ = Json::object();
        };
    }

    MapEntity SpawnMarker(const MapEntity& actor, std::string_view name)
    {
        auto marker = ZombieMarker(actor, name, "spawn_location");
        if (Field(actor, "script_string") == "zombie_chaser")
            marker.Set("script_string", "find_flesh");
        if (const auto target = Field(actor, "target"); !target.empty())
            marker.Set("target", std::string(target));
        return marker;
    }

    Converted Convert(MapEntities entities, std::string_view project, const Renames& animscripts, const ZoneNames& zoneNames,
                 std::string_view respawnExclusion)
    {
        return Converter(animscripts, respawnExclusion).Run(entities, project, Names(zoneNames.begin(), zoneNames.end()));
    }
}

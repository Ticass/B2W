#pragma once

#include "core/text.h"
#include "gsc/port/zone_scripts.h"
#include "mapents/map_entity.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::world { struct CollisionWorld; }

namespace w2b::mapents
{
    using util::Json;
    using Path = std::filesystem::path;

    MapEntities Parse(std::string_view text);

    struct Converted // the map's entities converted to T6, before BSP/ is written
    {
        MapEntities entities;
        Json spawns;  // spawns.json: attackers, defenders, FFA
        Json summary; // entities, dropped, renamed, spawn_points, ...
    };
    using Renames = std::map<std::string, std::string, std::less<>>;
    using ZoneNames = std::set<std::string>;
    Converted Convert(MapEntities entities, std::string_view project, const Renames& animscripts, const ZoneNames& zoneNames,
                      std::string_view respawnExclusion);

    struct MapEntityRequest
    {
        Path source, bsp;                      // the WaW .ents dump, the project's BSP folder
        std::string project;
        Renames animscripts;                   // traversal animscript renames from the paths pass
        std::optional<Path> zoneScript;        // the map script whose zone graph names the zones
        std::string respawnExclusion;
        const world::CollisionWorld* clip = nullptr;
    };

    struct MapEntityResult
    {
        Json summary;
        std::vector<std::string> scriptModels; // the script_model models placed, sorted
        std::optional<gsc::port::ZoneGraph> zoneGraph; // the map script's zone graph
    };

    // the map's entities converted and written to BSP/; throws std::invalid_argument on a malformed entity
    MapEntityResult ConvertMapEntities(const MapEntityRequest& request);
    // startZones: the zone graph's initial zones, none without a zone graph
    MapEntityResult Write(Converted converted, const Path& bsp, const world::CollisionWorld* clip, std::optional<std::vector<std::string>> startZones);
    std::vector<std::string> SynthesizeZones(MapEntities& entities, const world::CollisionWorld& clip, const Path& bsp, Json& summary);

    inline std::string_view Field(const MapEntity& entity, std::string_view key, std::string_view fallback = {})
    {
        return entity.Get(key, fallback);
    }
    std::string_view Field(const Json& entity, std::string_view key, std::string_view fallback = {});
    void Increment(Json& counters, std::string_view key);
    void NormalizeWallBuys(MapEntities& entities, Json& renamed);
    std::array<double, 3> Origin(const MapEntity& entity);
    std::array<double, 3> Origin(const Json& entity);
    long long BrushIndex(std::string_view model);
    MapEntity SpawnMarker(const MapEntity& actor, std::string_view name);
    inline constexpr std::string_view ZombieActor = "actor_zm_nuked_basic_01";
}

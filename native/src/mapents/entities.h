#pragma once

#include "core/pytext.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace w2b::world { struct CollisionWorld; }

namespace w2b::mapents
{
    using py::Json;
    using Path = std::filesystem::path;
    Json Parse(std::string_view text);
    Json Convert(Json entities, std::string_view project, const Json& animscripts, const Json& zoneNames,
                 std::string_view respawnExclusion);
    struct MapEntityRequest
    {
        Path source, bsp;                      // the WaW .ents dump, the project's BSP folder
        std::string project;
        Json animscripts;                      // traversal animscript renames from the paths pass
        std::optional<Path> zoneScript;        // the map script whose zone graph names the zones
        std::string respawnExclusion;
        const world::CollisionWorld* clip = nullptr;
    };

    // the map's entities converted and written to BSP/; throws std::invalid_argument on a malformed entity
    Json ConvertMapEntities(const MapEntityRequest& request);
    Json Write(Json converted, const Path& bsp, const world::CollisionWorld* clip, Json startZones);
    Json SynthesizeZones(Json& entities, const world::CollisionWorld& clip, const Path& bsp, Json& summary);

    std::string_view Field(const Json& entity, std::string_view key, std::string_view fallback = {});
    void Increment(Json& counters, std::string_view key);
    void NormalizeWallBuys(Json& entities, Json& renamed);
    std::array<double, 3> Origin(const Json& entity);
    long long BrushIndex(std::string_view model);
    Json SpawnMarker(const Json& actor, std::string_view name);
    inline constexpr std::string_view ZombieActor = "actor_zm_nuked_basic_01";
}

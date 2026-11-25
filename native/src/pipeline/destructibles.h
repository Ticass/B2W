#pragma once
// WaW destructibles: entities whose "destructibledef" names a piece table in
// the map's zones (breakable windows, exploding trucks). WaW's engine swaps
// each piece's model per damage stage; BO2's shows bones of one model instead,
// so the port keeps WaW's models and the compat script runs WaW's engine rules
// on the table generated here.

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct DestructiblePort
    {
        std::size_t placed = 0;                 // entities
        std::set<std::string> models, effects, sounds;
        std::vector<std::string> missing;       // definitions no zone of the map holds
        std::vector<std::string> zones;         // the zones searched
        std::string table;                      // the GSC data: piece tables and placements
    };

    // reads the placed destructibles of BSP/entities.json (and takes the key off: T6
    // would look for its own DestructibleDef asset) and their definitions in the map's zones
    DestructiblePort ReadDestructiblePort(const std::filesystem::path& entitiesJson, const std::vector<std::filesystem::path>& mapRoots);

    // maps/mp/waw/_waw2bo2_destructibles.gsc; empty tables when the map has none
    std::string DestructiblesSource(const DestructiblePort& port);
} // namespace w2b::pipeline

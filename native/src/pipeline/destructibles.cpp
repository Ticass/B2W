// WaW destructibles (see destructibles.h).
#include "destructibles.h"

#include "core/files.h"
#include "mapents/map_entity.h"
#include "modutil.h"
#include "oat/waw_tables.h"
#include "source_zones.h"

#include <map>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;
    using util::Json;

    namespace
    {
        std::string Quote(const Json& value)
        {
            return value.is_string() && !value.get<std::string>().empty() ? util::JsonDumps(Json(mod::Lower(value.get<std::string>())), 2) : "undefined";
        }

        // "x y z" as "x, y, z"
        std::string Comma(std::string text)
        {
            for (std::size_t at = 0; (at = text.find(' ', at)) != std::string::npos; at += 2)
                text.replace(at, 1, ", ");
            return text;
        }

        std::string Number(const Json& value)
        {
            return value.is_number_integer() ? std::to_string(value.get<long long>()) : util::FloatRepr(value.get<double>());
        }
    } // namespace

    DestructiblePort ReadDestructiblePort(const fs::path& entitiesJson, const std::vector<fs::path>& mapRoots)
    {
        DestructiblePort out;
        if (!mod::IsFile(entitiesJson))
            return out;
        auto entities = mapents::ReadMapEntities(entitiesJson);
        std::vector<std::pair<std::string, std::string>> placed; // origin, definition
        for (auto& entity : entities)
            if (const auto* name = entity.Find("destructibledef"))
            {
                placed.emplace_back(std::string(entity.Get("origin")), mod::Lower(*name));
                entity.Erase("destructibledef");
            }
        if (placed.empty())
            return out;
        mapents::WriteMapEntities(entitiesJson, entities);

        std::map<std::string, Json> definitions;
        for (const auto& zone : SourceZones(mapRoots))
            out.zones.push_back(files::Utf8(zone.filename()));
        for (const auto& zone : SourceZones(mapRoots))
        {
            const auto read = Json::parse(oat::ReadDestructibles({zone, {}})); // a local: items() of a temporary dangles
            for (const auto& [name, definition] : read.items())
                definitions.emplace(mod::Lower(name), definition);
        }

        std::vector<std::string> lines;
        std::set<std::string> used;
        for (const auto& [origin, name] : placed)
        {
            if (!definitions.contains(name))
            {
                out.missing.push_back(name);
                continue;
            }
            out.placed++;
            lines.push_back("    level.waw_destructibles_placed[level.waw_destructibles_placed.size] = maps\\mp\\waw\\_waw2bo2_compat::waw_destructible_place( (" +
                            Comma(origin) + "), " + Quote(Json(name)) + " );");
            used.insert(name);
        }
        for (const auto& name : used)
        {
            const auto& definition = definitions[name];
            const auto add = [](std::set<std::string>& set, const Json& value)
            {
                if (value.is_string() && !value.get<std::string>().empty())
                    set.insert(mod::Lower(value.get<std::string>()));
            };
            add(out.models, definition["baseModel"]);
            lines.push_back("    d = maps\\mp\\waw\\_waw2bo2_compat::waw_destructible_def( " + Quote(Json(name)) + ", " + Quote(definition["baseModel"]) + " );");
            for (const auto& piece : definition["pieces"])
            {
                lines.push_back("    p = d maps\\mp\\waw\\_waw2bo2_compat::waw_destructible_piece( " + Quote(piece["attachBone"]) + ", " + Number(piece["parentPiece"]) + ", " +
                                Number(piece["parentDamagePercent"]) + ", " + Number(piece["bulletDamageScale"]) + ", " + Number(piece["explosiveDamageScale"]) + ", " +
                                Number(piece["health"]) + " );");
                for (const auto& stage : piece["models"])
                {
                    if (!stage["model"].is_string())
                        break; // the stages end at the first without a model
                    add(out.models, stage["model"]);
                    add(out.effects, stage["breakEffect"]);
                    add(out.sounds, stage["breakSound"]);
                    add(out.sounds, Json(stage["loopSound"]));
                    for (const auto& model : stage["spawnModel"])
                        add(out.models, model);
                    lines.push_back("    p maps\\mp\\waw\\_waw2bo2_compat::waw_destructible_stage( " + Quote(stage["model"]) + ", " + Number(stage["breakHealth"]) + ", " +
                                    Number(stage["flags"]) + ", " + Quote(stage["breakEffect"]) + ", " + Quote(stage["breakSound"]) + ", " +
                                    Quote(stage["breakNotify"]) + ", " + Quote(stage["loopSound"]) + ", " + Quote(stage["spawnModel"][0]) + ", " +
                                    Quote(stage["spawnModel"][1]) + ", " + Quote(stage["spawnModel"][2]) + " );");
                }
            }
        }
        out.table = mod::Join(lines, "\n");
        return out;
    }

    std::string DestructiblesSource(const DestructiblePort& port)
    {
        return "// waw2bo2: the map's destructibles, WaW's piece tables and where they stand (generated).\n"
               "init()\n{\n    level.waw_destructible_defs = [];\n    level.waw_destructibles_placed = [];\n" +
               (port.table.empty() ? std::string() : port.table + "\n") + "}\n";
    }
} // namespace w2b::pipeline

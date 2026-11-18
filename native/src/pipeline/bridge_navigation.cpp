#include "pipeline/bridge_navigation.h"

#include "barriers/barriers.h"
#include "gsc/port/support.h"
#include "mapents/entities.h"
#include "paths/paths.h"
#include "pipeline/bridge_support.h"
#include "pipeline/finish.h"

namespace w2b::pipeline
{
    namespace
    {
        using py::Json;

        fs::path SourceEntities(const fs::path& gfx, const fs::path& stage, const std::vector<fs::path>& roots, const std::string& stem)
        {
            const auto name = files::FromUtf8(stem + ".ents");
            const auto primary = gfx.parent_path() / name;
            for (const auto& candidate : {primary, stage / "maps" / name})
                if (mod::IsFile(candidate))
                    return candidate;
            for (const auto& root : roots)
                for (const auto& folder : {"maps", "maps/mp"})
                    if (auto candidate = root / folder / name; mod::IsFile(candidate))
                        return candidate;
            return primary;
        }

        std::string TraversalRepr(const Json& traversals)
        {
            std::string text = "{";
            for (const auto& [source, target] : traversals.items())
            {
                if (text.size() > 1)
                    text += ", ";
                text += py::StringRepr(source) + ": " + py::StringRepr(target.get_ref<const std::string&>());
            }
            return text + '}';
        }

        Json StagePaths(const fs::path& source, const fs::path& project, const std::optional<fs::path>& bo2,
                        const Json& roots, Json& warnings, Json& errors)
        {
            if (!fs::exists(source))
            {
                errors.push_back("path nodes " + files::Utf8(source.filename()) + " missing (dump gameworldsp): zombies cannot path");
                return Json();
            }
            try
            {
                std::optional<fs::path> states;
                if (bo2)
                    if (auto path = *bo2 / "raw/animstatedefs/zm_nuked_basic.asd"; fs::exists(path))
                        states = std::move(path);
                auto result = paths::Stage(source, project / "BSP", project, states, bridge::PathList(roots));
                const auto& summary = result.at("summary");
                for (const auto& error : result.at("dogs").at("errors"))
                    warnings.push_back("UNCONVERTED_TRAVERSAL " + error.get<std::string>());
                const auto number = [&](const char* key) { return std::to_string(summary.at(key).get<std::size_t>()); };
                warnings.push_back("path nodes: " + number("nodes") + " nodes, " + number("links") + " links, vis "
                                   + number("vis_bytes") + " bytes; traversals " + TraversalRepr(summary.at("traversals")));
                return result;
            }
            catch (const paths::Error& error)
            {
                errors.push_back("path nodes: " + std::string(error.what()));
                return Json();
            }
        }

        void AppendSynthesizedZoneWarnings(const Json& entities, Json& warnings)
        {
            const auto found = entities.find("synthesized_zones");
            if (found == entities.end() || found->empty())
                return;
            const auto& summary = *found;
            warnings.push_back("ZONES_SYNTHESIZED the WaW map has no add_adjacent_zone graph: " + std::to_string(summary.at("groups").size())
                               + " spawner group(s) became map-wide zones, opened by the doors that add them in WaW (initial "
                               + mod::ListRepr(summary.at("initial").get<std::vector<std::string>>()) + ", "
                               + std::to_string(summary.at("adjacency").size()) + " door-unlocked)");
            const auto locked = summary.at("never_unlocked").get<std::vector<std::string>>();
            if (!locked.empty())
                warnings.push_back("ZONE_SPAWNERS_UNREACHED " + mod::ListRepr(locked) + ": no door or debris adds these spawners; "
                                   "they stay inactive unless the map script enables them");
        }

        void ReplacePerkMachines(const fs::path& bsp, const fs::path& bo2, const std::set<std::string>& machines, Json& models)
        {
            const auto path = bsp / "entities.json";
            try
            {
                gsc::port::stockperks::TranslateEntities(path, &bo2);
            }
            catch (const std::runtime_error& error)
            {
                throw StepError("StageError", "map entities: " + std::string(error.what()));
            }
            std::set<std::string, std::less<>> placed;
            const auto data = mod::Json(path);
            for (const auto& entity : data.at("entities"))
                if (const auto model = mapents::Field(entity, "model"); !model.empty() && !model.starts_with('*') && !machines.contains(std::string(model)))
                    placed.emplace(model);
            auto remaining = Json::array();
            for (const auto& model : models)
                if (placed.contains(model.get_ref<const std::string&>()))
                    remaining.push_back(model);
            models = std::move(remaining);
        }
    }

    Json StageBridgeNavigation(const Json& request, const fs::path& gfx, const fs::path& project,
                               const std::vector<fs::path>& roots, const world::CollisionWorld& clip)
    {
        auto stem = files::Utf8(gfx.filename());
        if (stem.ends_with(".gfx.bin"))
            stem.resize(stem.size() - std::string_view(".gfx.bin").size());
        const auto bsp = project / "BSP";
        const auto bo2 = bridge::OptionalPath(request.at("bo2_root"));
        const auto source = SourceEntities(gfx, files::FromUtf8(request.at("stage").get<std::string>()), roots, stem);
        auto warnings = Json::array(), errors = Json::array();
        const auto paths = StagePaths(gfx.parent_path() / files::FromUtf8(stem + ".paths.json"), project, bo2, request.at("traversal_roots"), warnings, errors);
        const auto machines = bo2 && request.value("bo2_stock_perks", false) ? gsc::port::stockperks::MachineModels(*bo2) : std::set<std::string>{};
        auto result = Json{{"entity_source", mod::Str(source)}, {"entities", Json::object()}, {"script_models", Json::array()}, {"has_entities", false},
                           {"zone_graph", nullptr}, {"dog_states", paths.is_null() ? Json::object() : paths.at("dogs").at("states")},
                           {"bo2_machine_models", machines}};
        if (fs::exists(source))
        {
            Json entityRequest{{"source", mod::Str(source)}, {"bsp", mod::Str(bsp)}, {"project", request.at("project")}, {"zoneNames", Json::array()},
                               {"startZones", nullptr}, {"animscripts", paths.is_null() ? Json::object() : paths.at("renamed")},
                               {"respawnExclusion", request.at("respawn_exclusion")}};
            if (const auto script = bridge::OptionalPath(request.at("waw_map_script")); script && fs::exists(*script))
                entityRequest["zoneScript"] = mod::Str(*script);
            auto entities = mapents::ConvertRequest(entityRequest, &clip);
            if (entities.contains("invalid"))
                throw StepError("StageError", "map entities: " + entities.at("invalid").get<std::string>());
            auto& summary = entities.at("summary");
            if (bo2)
            {
                try
                {
                    const auto boards = barriers::StageBoards(bsp, *bo2, &clip);
                    summary["barrier_alignment"] = boards.at("alignment");
                    summary["barrier_chunks"] = boards.at("chunks");
                }
                catch (const barriers::Error& error)
                {
                    throw StepError("StageError", "map entities: " + std::string(error.what()));
                }
            }
            AppendSynthesizedZoneWarnings(summary, warnings);
            if (bo2 && !machines.empty())
                ReplacePerkMachines(bsp, *bo2, machines, entities.at("scriptModels"));
            if (fs::exists(bsp / "paths.json"))
                for (const auto& note : paths::LinkWindowTraversals(bsp))
                    warnings.push_back(note);
            result["entities"] = std::move(summary);
            result["has_entities"] = true;
            result["script_models"] = std::move(entities.at("scriptModels"));
            result["zone_graph"] = entities.value("zoneGraph", Json());
        }
        else
            errors.push_back("map entities " + files::Utf8(source.filename()) + " missing");
        result["warnings"] = std::move(warnings);
        result["errors"] = std::move(errors);
        return result;
    }
}

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

        std::optional<paths::StagedPaths> StagePaths(const fs::path& source, const fs::path& project, const std::optional<fs::path>& bo2,
                                                     const std::vector<fs::path>& roots, std::vector<std::string>& warnings, std::vector<std::string>& errors)
        {
            if (!fs::exists(source))
            {
                errors.push_back("path nodes " + files::Utf8(source.filename()) + " missing (dump gameworldsp): zombies cannot path");
                return std::nullopt;
            }
            try
            {
                std::optional<fs::path> states;
                if (bo2)
                    if (auto path = *bo2 / "raw/animstatedefs/zm_nuked_basic.asd"; fs::exists(path))
                        states = std::move(path);
                auto result = paths::Stage(source, project / "BSP", project, states, roots);
                const auto& summary = result.summary;
                for (const auto& error : result.dogs.errors)
                    warnings.push_back("UNCONVERTED_TRAVERSAL " + error);
                const auto number = [&](const char* key) { return std::to_string(summary.at(key).get<std::size_t>()); };
                warnings.push_back("path nodes: " + number("nodes") + " nodes, " + number("links") + " links, vis "
                                   + number("vis_bytes") + " bytes; traversals " + TraversalRepr(summary.at("traversals")));
                return result;
            }
            catch (const paths::Error& error)
            {
                errors.push_back("path nodes: " + std::string(error.what()));
                return std::nullopt;
            }
        }

        void AppendSynthesizedZoneWarnings(const Json& entities, std::vector<std::string>& warnings)
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

        void ReplacePerkMachines(const fs::path& bsp, const fs::path& bo2, const std::set<std::string>& machines, std::vector<std::string>& models)
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
            for (const auto& entity : mapents::ReadMapEntities(path))
                if (const auto model = entity.Get("model"); !model.empty() && !model.starts_with('*') && !machines.contains(std::string(model)))
                    placed.emplace(model);
            std::erase_if(models, [&](const std::string& model) { return !placed.contains(model); });
        }
    }

    NavigationResult StageBridgeNavigation(const NavigationRequest& request, const fs::path& gfx, const fs::path& project,
                               const std::vector<fs::path>& roots, const world::CollisionWorld& clip)
    {
        auto stem = files::Utf8(gfx.filename());
        if (stem.ends_with(".gfx.bin"))
            stem.resize(stem.size() - std::string_view(".gfx.bin").size());
        const auto bsp = project / "BSP";
        const auto& bo2 = request.bo2Root;
        const auto source = SourceEntities(gfx, request.stage, roots, stem);
        NavigationResult result;
        auto& warnings = result.warnings;
        auto& errors = result.errors;
        const auto paths = StagePaths(gfx.parent_path() / files::FromUtf8(stem + ".paths.json"), project, bo2, request.traversalRoots, warnings, errors);
        const auto machines = bo2 && request.bo2StockPerks ? gsc::port::stockperks::MachineModels(*bo2) : std::set<std::string>{};
        result.entitySource = source;
        if (paths)
            result.dogStates = paths->dogs.states.get<std::map<std::string, std::string>>();
        result.machineModels = machines;
        if (fs::exists(source))
        {
            mapents::MapEntityRequest entityRequest{source, bsp, request.project, paths ? paths->renamed : mapents::Renames{},
                                                    std::nullopt, request.respawnExclusion, &clip};
            if (request.wawMapScript && fs::exists(*request.wawMapScript))
                entityRequest.zoneScript = request.wawMapScript;
            mapents::MapEntityResult entities;
            try
            {
                entities = mapents::ConvertMapEntities(entityRequest);
            }
            catch (const std::invalid_argument& error)
            {
                throw StepError("StageError", "map entities: " + std::string(error.what()));
            }
            auto& summary = entities.summary;
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
                ReplacePerkMachines(bsp, *bo2, machines, entities.scriptModels);
            if (fs::exists(bsp / "paths.json"))
                for (const auto& note : paths::LinkWindowTraversals(bsp))
                    warnings.push_back(note);
            result.entities = std::move(summary);
            result.hasEntities = true;
            result.scriptModels = std::move(entities.scriptModels);
            result.zoneGraph = std::move(entities.zoneGraph);
        }
        else
            errors.push_back("map entities " + files::Utf8(source.filename()) + " missing");
        return result;
    }
}

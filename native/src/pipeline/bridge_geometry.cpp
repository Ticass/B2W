#include "pipeline/bridge_geometry.h"

#include "extract/cache.h"
#include "extract/stockwaw.h"
#include "pipeline/bridge_support.h"
#include "pipeline/finish.h"
#include "pipeline/geometry_materials.h"
#include "world/bsp_visibility.h"
#include "world/collision_stage.h"
#include "world/collision_world.h"
#include "world/fbx_stage.h"
#include "world/probe_stage.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <map>

namespace w2b::pipeline
{
    namespace
    {
        using py::Json;
        using files::FromUtf8;

        class GeometryOutput
        {
        public:
            explicit GeometryOutput(fs::path bsp) : target(std::move(bsp))
            {
                fs::create_directories(target);
                static std::atomic<unsigned long long> counter{0};
                const auto prefix = "geometry-" + std::to_string(GetCurrentProcessId()) + "-";
                do
                    scratch = target / FromUtf8(prefix + std::to_string(counter++));
                while (!fs::create_directory(scratch));
            }

            GeometryOutput(const GeometryOutput&) = delete;
            GeometryOutput& operator=(const GeometryOutput&) = delete;
            ~GeometryOutput()
            {
                std::error_code error;
                for (const auto* output : outputs)
                    fs::remove(scratch / output, error);
                fs::remove(scratch, error);
            }

            const fs::path& Directory() const { return scratch; }

            void Publish(const char* file, const char* name, bool removeStale = false) const
            {
                const auto source = scratch / file;
                if (fs::is_regular_file(source))
                    extract::Publish(source, target / name, true);
                else if (removeStale)
                    fs::remove(target / name);
            }

        private:
            static constexpr std::array outputs{"world.fbx", "visibility.json", "shadowgeom.json", "terrain.fbx", "edges.bin",
                                                "brushes.json", "submodels.json", "staticmodels.json", "clipmaterials.json"};
            fs::path target, scratch;
        };

        void RemapProbes(world::RenderWorld& world, const Json& probes)
        {
            if (!probes.contains("kept"))
                return;
            std::map<int, int> mapping;
            for (const auto& [source, target] : probes.at("content").at("remap").items())
                mapping.emplace(std::stoi(source), target.get<int>());
            const auto remap = [&](int index)
            {
                const auto probe = mapping.find(index);
                return probe == mapping.end() ? 0 : probe->second;
            };
            for (auto& surface : world.surfaces)
                surface.reflectionProbe = remap(surface.reflectionProbe);
            const auto& staticProbes = probes.at("smodel_probes");
            for (std::size_t index = 0; index < staticProbes.size(); ++index)
                world.staticModels[index].reflectionProbe = static_cast<std::uint8_t>(remap(staticProbes.at(index).get<int>()));
        }

        std::string JoinNames(const std::set<std::string>& names)
        {
            std::string joined;
            for (const auto& name : names)
                joined += (joined.empty() ? "" : ", ") + name;
            return joined;
        }

        void SelectRenderSurfaces(world::RenderWorld& render, std::size_t clipModels, const GeometryMaterials& materials, Json& content, Json& warnings)
        {
            if (!render.brushModelCount && clipModels > 1)
                throw StepError("StageError", "render dump lacks brush ownership; re-extract gfxworld with the v4 T4 dumper");
            if (render.brushModelCount && *render.brushModelCount != clipModels)
                throw StepError("StageError", "render and collision brush model counts differ");

            std::size_t sourceBrushSurfaces = 0, keptBrushSurfaces = 0, skySurfaces = 0, editorSurfaces = 0;
            std::set<std::uint32_t> models;
            std::vector<world::WorldSurface> kept;
            kept.reserve(render.surfaces.size());
            for (auto& surface : render.surfaces)
            {
                sourceBrushSurfaces += surface.brushModel != 0;
                if (materials.sky.contains(surface.material))
                {
                    ++skySurfaces;
                    continue;
                }
                if (materials.tools.contains(surface.material))
                {
                    ++editorSurfaces;
                    continue;
                }
                if (surface.brushModel)
                {
                    if (surface.brushModel >= clipModels)
                        throw StepError("StageError", "render brush ownership exceeds collision submodel count");
                    ++keptBrushSurfaces;
                    models.insert(surface.brushModel);
                }
                kept.push_back(std::move(surface));
            }
            render.surfaces = std::move(kept);
            content["brush_render"] = {{"source_surfaces", sourceBrushSurfaces}, {"rendered_surfaces", keptBrushSurfaces}, {"models", models.size()}};
            if (skySurfaces)
                warnings.push_back(std::to_string(skySurfaces) + " sky surfaces (" + JoinNames(materials.sky) +
                                   ") removed from the render world; T6 draws the sky with the skybox xmodel");
            if (editorSurfaces)
                warnings.push_back(std::to_string(editorSurfaces) + " WaW editor-only surfaces (" + JoinNames(materials.tools) + ") removed from the render world");
        }

        Json WorldMetadata(const world::RenderWorld& world)
        {
            auto surfaces = Json::array(), models = Json::array();
            for (const auto& surface : world.surfaces)
                surfaces.push_back({surface.material, surface.lightmap});
            for (const auto& model : world.staticModels)
                models.push_back({model.name, model.origin, model.axis, model.scale, model.flags, model.cullDistance,
                                  model.primaryLight, model.reflectionProbe});
            return {{"surfaces", std::move(surfaces)}, {"static_models", std::move(models)}, {"shadow_lights", !world.shadowLights.empty()}};
        }

        std::string SheetCounts(const Json& sheets)
        {
            std::vector<std::pair<std::string, std::size_t>> counts;
            std::map<std::string, std::size_t> index;
            for (const auto& sheet : sheets)
            {
                const auto& name = sheet.get_ref<const std::string&>();
                auto [entry, inserted] = index.try_emplace(name, counts.size());
                if (inserted)
                    counts.emplace_back(name, 0);
                ++counts[entry->second].second;
            }
            std::ranges::stable_sort(counts, std::greater{}, [](const auto& entry) { return entry.second; });
            std::string summary;
            for (const auto& [name, count] : counts)
                summary += (summary.empty() ? "" : ", ") + name + ' ' + std::to_string(count);
            return summary;
        }

        Json StageCollision(const world::CollisionWorld& clip, const GeometryOutput& output, Json& warnings, Json& errors)
        {
            const auto collision = world::StageCollisionMesh(clip, output.Directory(), true);
            for (const auto* file : {"brushes.json", "submodels.json", "staticmodels.json"})
                output.Publish(file, file);
            output.Publish("terrain.fbx", "map_col.fbx");
            output.Publish("edges.bin", "collisionedges.bin");

            Json materials = Json::array();
            std::set<int> moving;
            for (const auto& slot : collision.at("slots"))
            {
                const int index = slot.at(0).get<int>(), model = slot.at(1).get<int>();
                if (index < 0)
                {
                    materials.push_back({{"fbx", "waw_collision"}, {"name", "waw_collision"}, {"contentFlags", 1}, {"surfaceFlags", 0}});
                    continue;
                }
                const auto& material = clip.materials.at(index);
                auto name = "clip_" + std::to_string(index);
                if (model)
                {
                    moving.insert(model);
                    name += "_bm" + std::to_string(model);
                }
                materials.push_back({{"fbx", name}, {"name", material.name}, {"model", model},
                                     {"contentFlags", std::bit_cast<std::int32_t>(material.contentFlags)},
                                     {"surfaceFlags", std::bit_cast<std::int32_t>(material.surfaceFlags)}});
            }
            const auto& records = collision.at("records");
            auto summary = records.at("brushes");
            summary["collision_triangle_materials"] = materials.size();
            summary["moving_terrain_models"] = moving;
            files::WriteText(output.Directory() / "clipmaterials.json", py::JsonDumps({{"materials", std::move(materials)}}, 1) + '\n');
            output.Publish("clipmaterials.json", "clipmaterials.json");
            summary["collision_triangles_noncolliding_dropped"] = collision.at("non_colliding_dropped");
            summary["collision_triangles_water_dropped"] = collision.at("water_dropped");
            const auto& sheets = collision.at("sheets");
            summary["collision_triangles_one_sided_removed"] = sheets.size();
            if (!sheets.empty())
                warnings.push_back("ONE_WAY_COLLISION_REMOVED " + std::to_string(sheets.size()) + " one-sided traversal triangles: "
                                   "collision omitted to allow passage in both directions (" + SheetCounts(sheets) + ')');
            summary.update(collision.at("edges"));
            summary["submodels"] = records.at("submodels");
            summary["empty_submodels"] = records.at("empty_submodels");
            const auto& staticModels = records.at("static_models");
            summary.update(staticModels);
            const auto outside = staticModels.at("static_model_triangles_outside_waw_bounds").get<std::size_t>();
            if (outside)
                errors.push_back(std::to_string(outside) + " static model collision triangles fall outside their WaW bounds: transform mismatch");
            return summary;
        }
    }

    Json StageBridgeGeometry(const Json& request)
    {
        const auto path = [&](const char* key) { return FromUtf8(request.at(key).get_ref<const std::string&>()); };
        const GeometryOutput output(path("project_root") / "BSP");
        auto render = world::ReadRenderWorld(path("gfx"), request.at("compensated").get<bool>());
        const auto clip = world::ReadCollisionWorld(path("clip"));
        const auto roots = bridge::PathList(request.at("roots"));
        std::optional<extract::StockWaw> stock;
        if (!render.layerData.empty() && std::ranges::any_of(render.surfaces, [](const auto& surface) { return surface.layerFormat == 0xff; }))
            stock = extract::StockWaw::FromJson(request.value("stock_waw", Json()));
        auto warnings = Json::array(), errors = Json::array(), content = Json::object();

        auto probes = world::StageProbes({{"source", request.at("probe_source")}, {"project", request.at("project_root")},
                                         {"image_roots", request.at("image_roots")}, {"compensated", request.at("compensated")}}, &render);
        auto visibility = world::AssignVisibility(render, path("visibility_source"), probes.value("cells", std::vector<std::vector<int>>{}));
        RemapProbes(render, probes);
        const auto materials = PlanGeometryMaterials(render, roots, stock ? &*stock : nullptr, warnings);
        SelectRenderSurfaces(render, clip.models.size(), materials, content, warnings);
        auto metadata = WorldMetadata(render);
        const auto shadowOnly = std::ranges::count_if(render.surfaces, [&](const auto& surface) { return materials.shadowOnly.contains(surface.material); });
        const auto meshes = world::WriteStagedWorld(render, output.Directory() / "world.fbx", materials.blendWeights, materials.shadowOnly, materials.ownLightCasters, &visibility);
        output.Publish("world.fbx", "map_gfx.fbx");
        for (const auto* file : {"visibility.json", "shadowgeom.json"})
            output.Publish(file, file, true);
        if (meshes.at("shadow_copies") != 0)
            content["shadow_occluder_copies"] = meshes.at("shadow_copies");
        content["visibility"] = meshes.at("visibility");
        content["shadow_only_surfaces"] = shadowOnly;
        if (!meshes.at("shadow_geometry").is_null())
            content["shadow_geometry"] = meshes.at("shadow_geometry");
        auto collision = StageCollision(clip, output, warnings, errors);
        return {{"content", std::move(content)}, {"collision", std::move(collision)}, {"warnings", std::move(warnings)}, {"errors", std::move(errors)},
                {"visibility_warning", meshes.at("visibility_warning")}, {"world", std::move(metadata)}, {"probes", std::move(probes)},
                {"sky", materials.skyImage ? Json(*materials.skyImage) : Json()},
                {"layered", !render.layerData.empty()}, {"stock_prepared", stock ? stock->PreparedJson() : Json()}};
    }

    Json StageBridgeGeometryRequest(const fs::path& path)
    {
        try
        {
            return StageBridgeGeometry(mod::Json(path));
        }
        catch (const StepError& error)
        {
            return {{"stage_error", error.what()}};
        }
    }
}

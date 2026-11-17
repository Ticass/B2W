#include "world/fbx_stage.h"

#include "core/files.h"
#include "world/bsp_visibility.h"
#include "world/fbx_writer.h"
#include "world/mesh_merge.h"
#include "world/shadow_geometry.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        using py::Json;

        void SelectSurfaces(RenderWorld& world, const Json& entries, bool assignedVisibility)
        {
            std::vector<WorldSurface> selected;
            selected.reserve(entries.size());
            for (const auto& row : entries)
            {
                auto surface = world.surfaces.at(row.at(0).get<std::size_t>());
                surface.primaryLight = row.at(1).get<int>();
                surface.reflectionProbe = row.at(2).get<int>();
                surface.layerFormat = row.at(3).get<int>();
                if (!assignedVisibility)
                    surface.cell = row.at(4).get<int>();
                selected.push_back(std::move(surface));
            }
            world.surfaces = std::move(selected);
        }

        Json Export(const RenderWorld& world, const files::fs::path& path, const RenderMeshes& result,
                    std::size_t copies)
        {
            if (path.has_parent_path())
                files::fs::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            WriteWorldFbx(output, {world.geometry.vertices, world.geometry.indices, world.layerData}, result.meshes);
            if (!output.flush())
                throw std::runtime_error("could not flush world FBX");
            return {{"surface_to_mesh", result.surfaceToMesh}, {"mesh_cells", result.cells},
                    {"shadow_copies", copies}, {"surfaces", world.surfaces.size() - copies},
                    {"vertices", world.geometry.vertices.size()}};
        }
    }

    Json StageWorldFbx(const files::fs::path& request)
    {
        const auto args = Json::parse(files::ReadText(request));
        auto world = ReadRenderWorld(files::FromUtf8(args.at("gfx").get<std::string>()),
                                     args.at("compensated").get<bool>());
        const bool assignedVisibility = args.contains("visibility_source");
        Visibility visibility;
        if (assignedVisibility)
            visibility = AssignVisibility(world, files::FromUtf8(args.at("visibility_source").get<std::string>()),
                                          args.value("cell_probes", std::vector<std::vector<int>>{}));
        if (args.contains("surfaces"))
            SelectSurfaces(world, args.at("surfaces"), assignedVisibility);
        else if (const auto limit = args.value("surface_limit", std::numeric_limits<std::size_t>::max());
                 limit < world.surfaces.size())
            world.surfaces.resize(limit);
        const auto copies = AddShadowCopies(world, args.value("own_light_casters", std::set<std::string>{}));
        const auto result = MergeSurfaces(world, args.value("blend_materials", std::set<std::string>{}),
                                         args.value("shadow_materials", std::set<std::string>{}));
        const auto folder = request.parent_path();
        auto report = Export(world, folder / "world.fbx", result, copies);
        if (assignedVisibility)
        {
            WriteVisibility(visibility, result.cells, folder / "visibility.json");
            report["visibility"] = std::move(visibility.summary);
            report["visibility_warning"] = std::move(visibility.warning);
            report["shadow_geometry"] = WriteShadowGeometry(world, result, folder / "shadowgeom.json");
        }
        return report;
    }

    Json ExportWorldFbx(const files::fs::path& source, const files::fs::path& output,
                        std::optional<std::size_t> surfaceLimit)
    {
        auto world = ReadRenderWorld(source, true);
        if (surfaceLimit && *surfaceLimit < world.surfaces.size())
            world.surfaces.resize(*surfaceLimit);
        return Export(world, output, MergeSurfaces(world), 0);
    }
}

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

    Json WriteStagedWorld(RenderWorld& world, const files::fs::path& fbx,
                          const std::set<std::string>& blendMaterials, const std::set<std::string>& shadowMaterials,
                          const std::set<std::string>& ownLightCasters, Visibility* visibility)
    {
        const auto copies = AddShadowCopies(world, ownLightCasters);
        const auto result = MergeSurfaces(world, blendMaterials, shadowMaterials);
        auto report = Export(world, fbx, result, copies);
        if (visibility)
        {
            const auto folder = fbx.parent_path();
            WriteVisibility(*visibility, result.cells, folder / "visibility.json");
            report["visibility"] = std::move(visibility->summary);
            report["visibility_warning"] = std::move(visibility->warning);
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

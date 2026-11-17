#include "world/inspect.h"

#include "world/collision_world.h"
#include "world/render_world.h"

#include <string_view>
#include <unordered_set>

namespace w2b::world
{
    py::Json InspectWorld(const std::filesystem::path& gfx, const std::filesystem::path& clip)
    {
        const auto world = ReadRenderWorld(gfx, true);
        const auto collision = ReadCollisionWorld(clip);
        std::unordered_set<std::string_view> materials;
        for (const auto& surface : world.surfaces)
            materials.insert(surface.material);
        return {{"world", world.name},
                {"vertices", world.geometry.vertices.size()},
                {"indices", world.geometry.indices.size()},
                {"triangles", world.geometry.indices.size() / 3},
                {"surfaces", world.surfaces.size()},
                {"materials", materials.size()},
                {"static_models", world.staticModels.size()},
                {"collision_vertices", collision.vertices.size()},
                {"collision_triangles", collision.indices.size() / 3},
                {"collision_brushes", collision.brushes.size()},
                {"collision_materials", collision.materials.size()}};
    }
}

#include "world/inspect.h"

#include "core/files.h"
#include "world/collision_world.h"
#include "world/render_world.h"

#include <set>
#include <string_view>
#include <unordered_set>

namespace w2b::world
{
    namespace
    {
        std::string LowerAssetName(std::string name)
        {
            for (auto& c : name)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            return name;
        }
    }

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

    py::Json InspectStaticModels(const std::filesystem::path& gfx, const std::filesystem::path& models)
    {
        const auto world = ReadRenderWorld(gfx, true);
        std::unordered_set<std::string> available;
        for (const auto& path : files::RGlob(models, ".gltf"))
        {
            auto name = LowerAssetName(files::Utf8(path.stem()));
            if (name.ends_with("_lod0"))
                name.resize(name.size() - 5);
            available.insert(std::move(name));
        }
        std::unordered_set<std::string_view> unique;
        std::unordered_set<std::string> covered;
        std::set<std::string_view> missing;
        for (const auto& model : world.staticModels)
        {
            if (!unique.insert(model.name).second)
                continue;
            auto name = LowerAssetName(model.name);
            if (available.contains(name))
                covered.insert(std::move(name));
            else
                missing.insert(model.name);
        }
        return {{"placements", world.staticModels.size()}, {"unique_models", unique.size()},
                {"available_unique_models", covered.size()}, {"missing_models", missing}};
    }
}

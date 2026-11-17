#include "world/shadow_geometry.h"

#include "core/files.h"

#include <unordered_map>

namespace w2b::world
{
    std::size_t AddShadowCopies(RenderWorld& world, const std::set<std::string>& materials)
    {
        if (materials.empty())
            return 0;
        std::unordered_map<int, std::size_t> byIndex;
        byIndex.reserve(world.surfaces.size());
        for (std::size_t i = 0; i < world.surfaces.size(); ++i)
            byIndex[world.surfaces[i].index] = i;
        const auto originalCount = world.surfaces.size();
        for (std::size_t light = 0; light < world.shadowLights.size(); ++light)
            for (const auto index : world.shadowLights[light].surfaces)
            {
                const auto entry = byIndex.find(index);
                if (entry == byIndex.end())
                    continue;
                const auto& surface = world.surfaces[entry->second];
                if (surface.brushModel || surface.primaryLight == static_cast<int>(light) ||
                    !materials.contains(surface.material))
                    continue;
                auto shadow = surface;
                shadow.primaryLight = static_cast<int>(light);
                shadow.shadowCopy = true;
                shadow.index = -1;
                world.surfaces.push_back(std::move(shadow));
            }
        return world.surfaces.size() - originalCount;
    }

    py::Json WriteShadowGeometry(const RenderWorld& world, const RenderMeshes& result, const std::filesystem::path& path)
    {
        if (world.shadowLights.empty())
            return {};
        std::unordered_map<int, int> surfaceToMesh;
        for (const auto& [surface, mesh] : result.surfaceToMesh)
            surfaceToMesh[surface] = mesh;
        std::size_t dropped = 0, castingMeshes = 0, castingModels = 0;
        auto lights = py::Json::array();
        for (const auto& light : world.shadowLights)
        {
            std::set<int> meshes;
            for (const auto surface : light.surfaces)
            {
                const auto entry = surfaceToMesh.find(surface);
                if (entry == surfaceToMesh.end())
                    ++dropped;
                else
                    meshes.insert(entry->second);
            }
            const std::set models(light.staticModels.begin(), light.staticModels.end());
            castingMeshes += meshes.size();
            castingModels += models.size();
            auto hulls = py::Json::array();
            for (const auto& hull : light.hulls)
            {
                auto axes = py::Json::array();
                for (const auto& [x, y, z, mid, half] : hull.axes)
                    axes.push_back({{"dir", {x, y, z}}, {"midPoint", mid}, {"halfSize", half}});
                hulls.push_back({{"kdopMidPoint", hull.midpoint}, {"kdopHalfSize", hull.halfSize},
                                 {"axes", std::move(axes)}});
            }
            lights.push_back({{"meshes", std::move(meshes)}, {"smodels", models}, {"hulls", std::move(hulls)}});
        }
        const auto lightCount = lights.size();
        files::WriteText(path, py::JsonDumps({{"lights", std::move(lights)}}, -1) + "\n");
        return {{"lights", lightCount}, {"casting_meshes", castingMeshes}, {"casting_smodels", castingModels},
                {"removed_source_surfaces", dropped}};
    }
}

#include "models/surface_fixes.h"

#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <map>

namespace w2b::models
{
    namespace
    {
        constexpr int MaxSortKey = 63;

        bool ModelMaterial(const Json& entry)
        {
            return entry.contains("t4_sort_key") && entry.value("t4_techset", std::string{}).starts_with("mc_");
        }

        bool Translucent(const Json& material)
        {
            const auto states = material.find("stateBits");
            return material.value("cameraRegion", std::string{}).ends_with("Trans") &&
                   states != material.end() && std::ranges::any_of(*states, [](const Json& state)
            {
                return state.value("colorWriteRgb", false) && !state.value("polymodeLine", false) &&
                       state.value("dstBlendRgb", std::string{"zero"}) != "zero";
            });
        }

        struct SortMaterial
        {
            Path path;
            Json material;
            int sourceKey;
            int originalKey;
            int key;
        };

        void RaiseKeys(const std::vector<SurfaceOrder>& orders, std::map<std::string, SortMaterial, std::less<>>& materials)
        {
            // Keep the model and surface traversal order while resolving each
            // material to its staged key only once.
            std::vector<std::pair<SortMaterial*, SortMaterial*>> comparisons;
            for (const auto& order : orders)
            {
                std::vector<SortMaterial*> previous;
                for (const auto& name : order)
                {
                    const auto found = materials.find(name);
                    if (found == materials.end())
                        continue;
                    auto* later = &found->second;
                    for (auto* earlier : previous)
                        if (earlier->sourceKey == later->sourceKey)
                            comparisons.emplace_back(earlier, later);
                    previous.push_back(later);
                }
            }
            for (int pass = 0; pass < MaxSortKey; ++pass)
            {
                bool moved = false;
                for (auto [earlier, later] : comparisons)
                    if (later->key < earlier->key && earlier->key < MaxSortKey)
                    {
                        later->key = earlier->key + 1;
                        moved = true;
                    }
                if (!moved)
                    break;
            }
        }
    }

    bool HasModelSortMaterials(const Json& materials)
    {
        return std::ranges::any_of(materials, ModelMaterial);
    }

    SurfaceOrder SurfaceMaterials(const Json& gltf)
    {
        SurfaceOrder names;
        if (!gltf.contains("materials") || !gltf.contains("meshes"))
            return names;
        const auto& materials = gltf.at("materials");
        MaterialNames seen;
        for (const auto& mesh : gltf.at("meshes"))
        {
            const auto primitives = mesh.find("primitives");
            if (primitives == mesh.end())
                continue;
            for (const auto& primitive : *primitives)
            {
                const auto index = primitive.find("material");
                if (index == primitive.end() || index->is_null())
                    continue;
                const auto& material = materials.at(index->get<std::size_t>());
                const auto name = material.find("name");
                if (name != material.end() && name->is_string() && seen.insert(name->get_ref<const std::string&>()).second)
                    names.push_back(name->get_ref<const std::string&>());
            }
        }
        return names;
    }

    Json ApplySurfaceOrder(const Path& project, const Json& materials, const std::vector<SurfaceOrder>& orders)
    {
        auto entries = Json::object();
        for (const auto& entry : materials)
            if (ModelMaterial(entry))
                entries[entry.value("source", entry.at("name").get<std::string>())] = entry;
        std::map<std::string, SortMaterial, std::less<>> eligible;
        for (const auto& [name, entry] : entries.items())
        {
            const auto path = project / files::FromUtf8(entry.at("file").get_ref<const std::string&>());
            if (!files::fs::is_regular_file(path))
                continue;
            auto definition = weapons::ReadAssetJson(path);
            if (!definition.contains("sortKey") || !Translucent(definition))
                continue;
            const auto key = definition.at("sortKey").get<int>();
            eligible.emplace(name, SortMaterial{path, std::move(definition), entry.at("t4_sort_key").get<int>(), key, key});
        }
        RaiseKeys(orders, eligible);
        auto changed = Json::object();
        for (const auto& [name, entry] : entries.items())
        {
            const auto it = eligible.find(name);
            if (it == eligible.end() || it->second.key == it->second.originalKey)
                continue;
            auto& value = it->second;
            value.material["sortKey"] = value.key;
            ReplaceStagedText(value.path, py::JsonDumps(value.material, 2) + "\n");
            changed[name] = {value.originalKey, value.key};
        }
        return changed;
    }
}

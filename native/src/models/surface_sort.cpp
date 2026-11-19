#include "models/surface_fixes.h"

#include "core/files.h"
#include "materials/t6_material.h"
#include "weapons/stage.h"

#include <algorithm>
#include <map>

namespace w2b::models
{
    namespace
    {
        constexpr int MaxSortKey = 63;

        bool ModelMaterial(const SurfaceMaterial& entry)
        {
            return entry.t4SortKey && entry.t4Techset.starts_with("mc_");
        }

        bool Translucent(const materials::T6Material& material)
        {
            return material.cameraRegion.ends_with("Trans") && std::ranges::any_of(material.stateBits, [](const materials::T6StateBits& state)
            {
                return state.colorWriteRgb && !state.polymodeLine && state.dstBlendRgb != "zero";
            });
        }

        struct SortMaterial
        {
            Path path;
            materials::T6Material material;
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

    bool HasModelSortMaterials(const std::vector<SurfaceMaterial>& materials)
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

    std::vector<SortKeyChange> ApplySurfaceOrder(const Path& project, const std::vector<SurfaceMaterial>& materials, const std::vector<SurfaceOrder>& orders)
    {
        // by WaW name, in first-seen order; a later record of the same name replaces the earlier one
        std::vector<std::pair<std::string, const SurfaceMaterial*>> entries;
        for (const auto& entry : materials)
            if (ModelMaterial(entry))
            {
                const auto known = std::ranges::find(entries, entry.wawName, &std::pair<std::string, const SurfaceMaterial*>::first);
                if (known != entries.end())
                    known->second = &entry;
                else
                    entries.emplace_back(entry.wawName, &entry);
            }
        std::map<std::string, SortMaterial, std::less<>> eligible;
        for (const auto& [name, entry] : entries)
        {
            const auto path = project / files::FromUtf8(entry->file);
            if (!files::fs::is_regular_file(path))
                continue;
            auto definition = materials::ReadT6Material(path);
            if (!Translucent(definition))
                continue;
            const auto key = static_cast<int>(definition.sortKey);
            eligible.emplace(name, SortMaterial{path, std::move(definition), *entry->t4SortKey, key, key});
        }
        RaiseKeys(orders, eligible);
        std::vector<SortKeyChange> changed;
        for (const auto& [name, entry] : entries)
        {
            const auto it = eligible.find(name);
            if (it == eligible.end() || it->second.key == it->second.originalKey)
                continue;
            auto& value = it->second;
            value.material.sortKey = static_cast<unsigned>(value.key);
            ReplaceStagedText(value.path, materials::T6MaterialText(value.material));
            changed.push_back({name, value.originalKey, value.key});
        }
        return changed;
    }
}

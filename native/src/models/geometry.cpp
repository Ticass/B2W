#include "models/models.h"

#include "weapons/skeleton.h"
#include "weapons/weapon.h"

#include <algorithm>
#include <set>

namespace w2b::models
{
    bool StripBadSkins(Json& geometry)
    {
        try
        {
            weapons::PrepareSkeleton(geometry, false);
            return false;
        }
        catch (const weapons::WeaponError&)
        {
            geometry.erase("skins");
            if (const auto nodes = geometry.find("nodes"); nodes != geometry.end())
                for (auto& node : *nodes)
                    node.erase("skin");
            if (const auto meshes = geometry.find("meshes"); meshes != geometry.end())
                for (auto& mesh : *meshes)
                    if (const auto primitives = mesh.find("primitives"); primitives != mesh.end())
                        for (auto& primitive : *primitives)
                            if (const auto attributes = primitive.find("attributes"); attributes != primitive.end())
                                for (auto field = attributes->begin(); field != attributes->end();)
                                {
                                    if (field.key().starts_with("JOINTS_") || field.key().starts_with("WEIGHTS_"))
                                        field = attributes->erase(field);
                                    else
                                        ++field;
                                }
            return true;
        }
    }

    std::size_t NeutralizeTransforms(Json& geometry)
    {
        const auto nodes = geometry.find("nodes");
        if (nodes == geometry.end())
            return 0;
        const auto numeric = [](const Json& value) { return value.is_number() || value.is_boolean(); };
        static const std::array<std::pair<std::string_view, Json>, 3> neutral{{
            {"translation", {0.0, 0.0, 0.0}}, {"rotation", {0.0, 0.0, 0.0, 1.0}}, {"scale", {1.0, 1.0, 1.0}},
        }};
        std::size_t fixed = 0;
        for (auto& node : *nodes)
            for (const auto& [name, base] : neutral)
            {
                const auto transform = node.find(name);
                if (transform == node.end() || std::ranges::all_of(*transform, numeric))
                    continue;
                auto result = Json::array();
                for (std::size_t i = 0; i < std::min(base.size(), transform->size()); ++i)
                    result.push_back(numeric(transform->at(i)) ? transform->at(i) : base.at(i));
                *transform = std::move(result);
                ++fixed;
            }
        return fixed;
    }

    std::vector<std::string> DropShadowSurfaces(Json& geometry, const std::function<bool(const std::string&)>& shadowOnly)
    {
        const auto meshes = geometry.find("meshes");
        if (meshes == geometry.end())
            return {};
        std::vector<std::string> names;
        if (const auto materials = geometry.find("materials"); materials != geometry.end())
            for (const auto& material : *materials)
                names.push_back(material.value("name", std::string{}));
        const auto size = static_cast<std::int64_t>(names.size());
        const auto byIndex = [&](std::int64_t index) -> const std::string& {
            return names.at(static_cast<std::size_t>(index < 0 ? size + index : index));
        };
        std::set<Json> used, hidden;
        for (const auto& mesh : *meshes)
            if (const auto primitives = mesh.find("primitives"); primitives != mesh.end())
                for (const auto& primitive : *primitives)
                    used.insert(primitive.value("material", Json()));
        for (const auto& material : used)
            if (material.is_number_integer() && material.get<std::int64_t>() < size && shadowOnly(byIndex(material.get<std::int64_t>())))
                hidden.insert(material);
        if (hidden.empty() || hidden == used)
            return {};
        for (auto& mesh : *meshes)
        {
            auto filtered = Json::array();
            if (const auto primitives = mesh.find("primitives"); primitives != mesh.end())
                for (auto& primitive : *primitives)
                    if (!hidden.contains(primitive.value("material", Json())))
                        filtered.push_back(std::move(primitive));
            mesh["primitives"] = std::move(filtered);
        }
        if (const auto nodes = geometry.find("nodes"); nodes != geometry.end())
            for (auto& node : *nodes)
                if (const auto mesh = node.find("mesh"); mesh != node.end() &&
                    meshes->at(mesh->get<std::size_t>()).at("primitives").empty())
                    node.erase(mesh);
        std::vector<std::string> removed;
        for (const auto& material : hidden)
            removed.push_back(byIndex(material.get<std::int64_t>()));
        std::ranges::sort(removed);
        return removed;
    }
}

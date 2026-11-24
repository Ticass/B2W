#include "models/models.h"

#include "weapons/skeleton.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace w2b::models
{
    bool StripBadSkins(Json& geometry, bool rigid)
    {
        // An animated model keeps its root bones, however many (T6 allows several, as WaW does).
        const auto skins = weapons::SkinRoots(geometry);
        if (std::ranges::all_of(skins, [&](const auto& roots) { return roots.size() == 1 || (!rigid && !roots.empty()); }))
            return false;
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

    std::size_t RepairHitboxes(Json& model)
    {
        // The dump writes a non-finite float as null. WaW tests a bone's sphere before its box, and a NaN or
        // infinite sphere rejects nothing, so the box alone decides: the sphere becomes the box's own. Without
        // a usable box the bone has no hitbox (radius 0).
        const auto info = model.find("boneInfo");
        if (info == model.end() || !info->is_array())
            return 0;
        const auto finite = [](const Json& value) {
            return value.is_array() ? std::ranges::all_of(value, [](const Json& v) { return v.is_number(); }) : value.is_number();
        };
        std::size_t fixed = 0;
        for (auto& bone : *info)
        {
            if (std::ranges::all_of(std::array{"mins", "maxs", "offset", "radiusSquared"},
                                    [&](const char* key) { return !bone.contains(key) || finite(bone.at(key)); }))
                continue;
            ++fixed;
            if (!bone.contains("mins") || !bone.contains("maxs") || !finite(bone.at("mins")) || !finite(bone.at("maxs")) ||
                bone.at("mins").size() != 3 || bone.at("maxs").size() != 3)
            {
                bone["mins"] = bone["maxs"] = bone["offset"] = Json::array({0.0, 0.0, 0.0});
                bone["radiusSquared"] = 0.0;
                continue;
            }
            const auto mins = bone.at("mins").get<Vec3>(), maxs = bone.at("maxs").get<Vec3>();
            if (!bone.contains("offset") || !finite(bone.at("offset")) || bone.at("offset").size() != 3)
                bone["offset"] = Vec3{(mins[0] + maxs[0]) / 2, (mins[1] + maxs[1]) / 2, (mins[2] + maxs[2]) / 2};
            const auto offset = bone.at("offset").get<Vec3>();
            double radius = 0;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const auto reach = std::max(std::abs(mins[axis] - offset[axis]), std::abs(maxs[axis] - offset[axis]));
                radius += reach * reach;
            }
            // a box of stray values (Return to Stairway to Hell) can exceed a float: the largest one still rejects nothing
            bone["radiusSquared"] = std::min(radius, static_cast<double>(std::numeric_limits<float>::max()));
        }
        return fixed;
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

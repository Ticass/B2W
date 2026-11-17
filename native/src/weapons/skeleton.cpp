#include "weapons/skeleton.h"

#include "weapons/weapon.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>

namespace w2b::weapons
{
    namespace
    {
        using Parents = std::map<std::size_t, std::size_t>;
        constexpr std::array TransformKeys{"translation", "rotation", "scale", "matrix"};

        Parents FindParents(const Json& nodes)
        {
            Parents parents;
            for (std::size_t i = 0; i < nodes.size(); ++i)
                if (const auto children = nodes.at(i).find("children"); children != nodes.at(i).end())
                    for (const auto& child : *children)
                        parents[child.get<std::size_t>()] = i;
            return parents;
        }

        std::optional<std::size_t> Parent(const Parents& parents, std::size_t node)
        {
            const auto entry = parents.find(node);
            return entry == parents.end() ? std::nullopt : std::optional{entry->second};
        }

        bool EqualTransform(const Json& left, const Json& right)
        {
            return std::ranges::all_of(TransformKeys, [&](const auto* key) {
                const auto a = left.find(key);
                const auto b = right.find(key);
                if (a == left.end() || b == right.end())
                    return a == left.end() && b == right.end();
                return *a == *b;
            });
        }

        void RemoveNode(Json& list, std::size_t node)
        {
            const auto entry = std::ranges::find(list, Json(node));
            if (entry != list.end())
                list.erase(entry);
        }

        bool MergeRoots(Json& gltf, const std::vector<std::size_t>& roots)
        {
            auto& nodes = gltf.at("nodes");
            const auto parents = FindParents(nodes);
            const auto commonParent = Parent(parents, roots.front());
            const auto& first = nodes.at(roots.front());
            if (!std::ranges::all_of(roots, [&](auto root) {
                    return Parent(parents, root) == commonParent && EqualTransform(first, nodes.at(root));
                }))
                return false;
            const auto origin = std::ranges::find_if(roots, [&](auto root) {
                return Field(nodes.at(root), "name") == "tag_origin";
            });
            const auto primary = origin == roots.end() ? roots.front() : *origin;
            for (const auto root : roots)
            {
                if (root == primary)
                    continue;
                for (const auto* key : TransformKeys)
                    nodes.at(root).erase(key);
                if (commonParent)
                    RemoveNode(nodes.at(*commonParent).at("children"), root);
                nodes.at(primary)["children"].push_back(root);
                if (const auto scenes = gltf.find("scenes"); scenes != gltf.end())
                    for (auto& scene : *scenes)
                        if (const auto children = scene.find("nodes"); children != scene.end())
                            RemoveNode(*children, root);
            }
            return true;
        }
    }

    Json PrepareSkeleton(Json& gltf, bool rigid)
    {
        const auto skins = gltf.find("skins");
        auto merged = Json::array();
        if (skins == gltf.end() || skins->empty())
            return merged;
        const auto nodes = gltf.find("nodes");
        const auto parents = nodes == gltf.end() ? Parents{} : FindParents(*nodes);
        for (const auto& skin : *skins)
        {
            const auto joints = skin.value("joints", std::set<std::size_t>{});
            std::vector<std::size_t> roots;
            for (const auto joint : joints)
            {
                const auto parent = Parent(parents, joint);
                if (!parent || !joints.contains(*parent))
                    roots.push_back(joint);
            }
            if (roots.size() == 1)
                continue;
            if (roots.empty() || !rigid || !MergeRoots(gltf, roots))
                throw WeaponError("multiple/absent skin roots: skeleton preserved at source, not stripped");
            merged.clear();
            for (const auto root : roots)
                merged.push_back(gltf.at("nodes").at(root).value("name", Json()));
        }
        return merged;
    }

    std::size_t LodVertices(const Json& gltf)
    {
        const auto meshes = gltf.find("meshes");
        if (meshes == gltf.end())
            return 0;
        std::set<std::size_t> positions;
        for (const auto& mesh : *meshes)
            for (const auto& primitive : mesh.at("primitives"))
                positions.insert(primitive.at("attributes").at("POSITION").get<std::size_t>());
        std::size_t vertices = 0;
        for (const auto accessor : positions)
            vertices += gltf.at("accessors").at(accessor).at("count").get<std::size_t>();
        return vertices;
    }
}

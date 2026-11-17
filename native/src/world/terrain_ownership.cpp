#include "world/terrain_ownership.h"

#include <cstddef>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        class TerrainClaims
        {
        public:
            explicit TerrainClaims(const CollisionWorld& world)
                : world_(world), owners_(world.indices.size() / 3),
                  finishedBy_(world.terrainNodes.size()), active_(world.terrainNodes.size())
            {
            }

            void Add(std::uint32_t model, const TerrainRoot& root)
            {
                if (!root.contents || !root.count)
                    return;
                CheckNodeRange(root.firstNode, root.count);
                pending_.clear();
                for (std::size_t node = root.firstNode + static_cast<std::size_t>(root.count); node > root.firstNode;)
                    pending_.push_back({--node, false});
                while (!pending_.empty())
                {
                    const auto [index, leaving] = pending_.back();
                    pending_.pop_back();
                    if (leaving)
                    {
                        active_[index] = false;
                        finishedBy_[index] = model;
                        continue;
                    }
                    if (active_[index])
                        throw std::runtime_error("Invalid or cyclic moving terrain AABB tree");
                    if (finishedBy_[index] == model)
                        continue;
                    const auto& node = world_.terrainNodes[index];
                    if (node.children)
                        AddChildren(index, node);
                    else
                    {
                        ClaimPartition(model, node.firstChildOrPartition);
                        finishedBy_[index] = model;
                    }
                }
            }

            std::vector<std::uint32_t> TakeOwners() { return std::move(owners_); }

        private:
            struct Visit
            {
                std::size_t node;
                bool leaving;
            };

            void CheckNodeRange(std::size_t first, std::size_t count) const
            {
                if (first >= world_.terrainNodes.size() || count > world_.terrainNodes.size() - first)
                    throw std::runtime_error("Invalid or cyclic moving terrain AABB tree");
            }

            void AddChildren(std::size_t parent, const TerrainNode& node)
            {
                const auto first = static_cast<std::size_t>(node.firstChildOrPartition);
                CheckNodeRange(first, node.children);
                active_[parent] = true;
                pending_.push_back({parent, true});
                for (std::size_t index = first + node.children; index > first;)
                    pending_.push_back({--index, false});
            }

            void ClaimPartition(std::uint32_t model, std::int32_t index)
            {
                if (index < 0 || static_cast<std::size_t>(index) >= world_.partitions.size())
                    throw std::runtime_error("Moving terrain references an invalid partition");
                const auto [first, count] = world_.partitions[index];
                if (first > owners_.size() || count > owners_.size() - first)
                    throw std::runtime_error("Moving terrain partition exceeds the triangle array");
                const auto end = static_cast<std::size_t>(first) + count;
                for (std::size_t triangle = first; triangle < end; ++triangle)
                {
                    auto& owner = owners_[triangle];
                    if (owner && owner != model)
                        throw std::runtime_error("Terrain triangle belongs to multiple moving models");
                    owner = model;
                }
            }

            const CollisionWorld& world_;
            std::vector<std::uint32_t> owners_;
            std::vector<std::uint32_t> finishedBy_;
            std::vector<bool> active_;
            std::vector<Visit> pending_;
        };
    }

    std::vector<std::uint32_t> TerrainOwners(const CollisionWorld& world)
    {
        if (!world.modelRoots)
        {
            if (!world.partitions.empty() && world.models.size() > 1)
                throw std::runtime_error("Collision dump lacks moving terrain ownership; re-extract with the updated WaW extractor");
            return std::vector<std::uint32_t>(world.indices.size() / 3);
        }

        TerrainClaims claims(world);
        for (std::uint32_t model = 1; model < world.modelRoots->size(); ++model)
            claims.Add(model, (*world.modelRoots)[model]);
        return claims.TakeOwners();
    }
}

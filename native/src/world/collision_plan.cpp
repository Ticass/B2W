#include "world/collision_plan.h"

#include "world/one_way_collision.h"
#include "world/terrain_ownership.h"

#include <algorithm>
#include <map>

namespace w2b::world
{
    CollisionPlan PlanCollision(const CollisionWorld& world, std::optional<std::size_t> triangleLimit)
    {
        const auto total = world.indices.size() / 3;
        const auto count = std::min(total, triangleLimit.value_or(total));
        const auto owners = TerrainOwners(world);
        CollisionPlan plan;
        if (world.triangleMaterials.empty())
        {
            plan.triangleSlots.assign(count, 0);
            plan.slots.push_back({-1, 0});
            return plan;
        }
        // Resolve ownership and opposing faces against the full collision world.
        plan.oneWayTriangles = OneWayTriangles(world);
        plan.oneWayTriangles.erase(std::ranges::lower_bound(plan.oneWayTriangles, count), plan.oneWayTriangles.end());
        plan.triangleSlots.assign(count, -1);
        std::map<std::pair<std::uint16_t, std::uint32_t>, int> slotOfMaterial;
        auto oneWay = plan.oneWayTriangles.begin();
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto material = world.triangleMaterials[index];
            const auto contents = material == 0xffff ? 0 : world.materials.at(material).contentFlags;
            const auto owner = owners.at(index);
            const bool nonColliding = !contents;
            const bool worldWater = !owner && (contents & ~nonCollidingContents) == waterContents;
            const bool removedSheet = oneWay != plan.oneWayTriangles.end() && *oneWay == index;
            if (removedSheet)
                ++oneWay;
            plan.nonCollidingDropped += nonColliding;
            plan.waterDropped += worldWater;
            if (nonColliding || worldWater || removedSheet)
                continue;
            const auto [slot, added] = slotOfMaterial.try_emplace({material, owner}, static_cast<int>(plan.slots.size()));
            if (added)
                plan.slots.push_back({material, owner});
            plan.triangleSlots[index] = slot->second;
        }
        return plan;
    }
}

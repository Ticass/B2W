#include "world/collision_plan.h"

#include "world/one_way_collision.h"
#include "world/terrain_ownership.h"

#include <map>

namespace w2b::world
{
    CollisionPlan PlanCollision(const CollisionWorld& world)
    {
        constexpr std::uint32_t water = 0x20;
        const auto owners = TerrainOwners(world);
        CollisionPlan plan;
        if (world.triangleMaterials.empty())
        {
            plan.triangleSlots.assign(world.indices.size() / 3, 0);
            plan.slots.push_back({-1, 0});
            return plan;
        }
        plan.oneWayTriangles = OneWayTriangles(world);
        plan.triangleSlots.assign(world.triangleMaterials.size(), -1);
        std::map<std::pair<std::uint16_t, std::uint32_t>, int> slotOfMaterial;
        auto oneWay = plan.oneWayTriangles.begin();
        for (std::size_t index = 0; index < world.triangleMaterials.size(); ++index)
        {
            const auto material = world.triangleMaterials[index];
            const auto contents = material == 0xffff ? 0 : world.materials.at(material).contentFlags;
            const auto owner = owners.at(index);
            const bool nonColliding = !contents;
            const bool worldWater = !owner && (contents & ~nonCollidingContents) == water;
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

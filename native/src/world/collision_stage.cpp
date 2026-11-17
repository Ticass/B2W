#include "world/collision_stage.h"

#include "world/collision_plan.h"
#include "world/fbx_writer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        py::Json WriteCollisionEdges(const std::filesystem::path& path, const CollisionWorld& world,
                                     const CollisionPlan& plan)
        {
            if (world.edgeWalkable.empty())
                return {{"collision_edges_source", "none (pre-v6 dump; bridge keeps every edge walkable)"}};

            std::ofstream output(path, std::ios::binary);
            const auto count = static_cast<std::uint32_t>(std::ranges::count_if(plan.triangleSlots,
                [](int slot) { return slot >= 0; }));
            output.write(reinterpret_cast<const char*>(&count), sizeof(count));
            std::size_t walkable = 0;
            for (std::size_t triangle = 0; triangle < plan.triangleSlots.size(); ++triangle)
            {
                if (plan.triangleSlots[triangle] < 0)
                    continue;
                std::array<ClipPoint, 3> corners;
                unsigned char bits = 0;
                for (std::size_t edge = 0; edge < corners.size(); ++edge)
                {
                    const auto bit = triangle * 3 + edge;
                    corners[edge] = world.vertices.at(world.indices.at(bit));
                    bits |= static_cast<unsigned char>(((world.edgeWalkable.at(bit / 8) >> (bit % 8)) & 1) << edge);
                }
                walkable += std::popcount(bits);
                output.write(reinterpret_cast<const char*>(corners.data()), sizeof(corners));
                output.put(static_cast<char>(bits));
            }
            if (!output.flush())
                throw std::runtime_error("could not write collision edge records");
            return {{"collision_edges_source", "waw"}, {"collision_edges_walkable", walkable},
                    {"collision_edges_total", static_cast<std::uint64_t>(count) * 3}};
        }
    }

    py::Json StageCollisionMesh(const std::filesystem::path& source, const std::filesystem::path& output)
    {
        const auto world = ReadCollisionWorld(source);
        const auto plan = PlanCollision(world);
        {
            std::ofstream fbx(output / "terrain.fbx", std::ios::binary);
            WriteCollisionFbx(fbx, world, plan);
            if (!fbx.flush())
                throw std::runtime_error("could not flush collision FBX");
        }

        auto slots = py::Json::array();
        for (const auto& slot : plan.slots)
            slots.push_back({slot.material, slot.model});
        auto sheets = py::Json::array();
        for (const auto id : plan.oneWayTriangles)
            sheets.push_back(world.materials.at(world.triangleMaterials.at(id)).name);
        return {{"slots", std::move(slots)}, {"sheets", std::move(sheets)},
                {"non_colliding_dropped", plan.nonCollidingDropped}, {"water_dropped", plan.waterDropped},
                {"edges", WriteCollisionEdges(output / "edges.bin", world, plan)}};
    }
}

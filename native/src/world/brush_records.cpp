#include "world/collision_records.h"

#include "world/geometry_math.h"

#include <algorithm>
#include <bit>
#include <span>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        using py::Json;

        std::vector<Vec3> BrushVertices(const ClipBrush& brush)
        {
            std::vector<Vec3> vertices;
            const auto& planes = brush.planes;
            for (std::size_t a = 0; a < planes.size(); ++a)
                for (std::size_t b = a + 1; b < planes.size(); ++b)
                    for (std::size_t c = b + 1; c < planes.size(); ++c)
                    {
                        const auto point = Solve3({ToVec3(planes[a].normal), ToVec3(planes[b].normal),
                                                   ToVec3(planes[c].normal)},
                                                  {planes[a].distance, planes[b].distance, planes[c].distance});
                        if (!point || std::ranges::any_of(planes, [&](const BrushPlane& plane)
                            { return Dot(ToVec3(plane.normal), *point) > plane.distance + 0.01; }))
                            continue;
                        if (std::ranges::none_of(vertices, [&](const Vec3& vertex)
                            {
                                const auto delta = Subtract(*point, vertex);
                                return Dot(delta, delta) < 1e-8;
                            }))
                            vertices.push_back(*point);
                    }
            if (vertices.size() < 4)
                throw std::runtime_error("collision brush has no valid convex vertex hull");
            return vertices;
        }

        Json BrushRecord(const ClipBrush& brush, const std::vector<ClipMaterial>& materials)
        {
            auto axial = Json::array();
            auto sides = Json::array();
            for (std::size_t i = 0; i < brush.planes.size(); ++i)
            {
                const auto& plane = brush.planes[i];
                const bool hasMaterial = plane.material < materials.size();
                const auto contents = std::bit_cast<std::int32_t>(
                    hasMaterial ? materials[plane.material].contentFlags : brush.contents);
                const auto flags = std::bit_cast<std::int32_t>(
                    hasMaterial ? materials[plane.material].surfaceFlags : 0u);
                if (i < 6)
                    axial.push_back({contents, flags});
                else
                    sides.push_back({plane.normal[0], plane.normal[1], plane.normal[2], plane.distance, contents, flags});
            }
            auto verts = brush.vertices.empty() ? Json(BrushVertices(brush)) : Json(brush.vertices);
            return {{"mins", brush.mins}, {"maxs", brush.maxs}, {"contents", std::bit_cast<std::int32_t>(brush.contents)},
                    {"axial", std::move(axial)}, {"sides", std::move(sides)}, {"verts", std::move(verts)}};
        }

        bool InsideModel(const ClipBrush& brush, const ClipModel& model)
        {
            for (std::size_t axis = 0; axis < brush.mins.size(); ++axis)
                if (!(model.mins[axis] - 1.0 <= brush.mins[axis] && brush.maxs[axis] <= model.maxs[axis] + 1.0))
                    return false;
            return true;
        }
    }

    CollisionRecords BuildWorldBrushes(const CollisionWorld& world)
    {
        std::vector<bool> listed(world.brushes.size());
        std::size_t owned = 0;
        for (std::size_t i = 1; i < world.models.size(); ++i)
            for (const auto index : world.models[i].brushes)
            {
                listed.at(index) = true;
                ++owned;
            }
        std::vector<std::size_t> worldIds;
        if (world.models.empty())
            for (std::size_t i = 0; i < world.brushes.size(); ++i)
                worldIds.push_back(i);
        else
            for (const auto index : world.models.front().brushes)
                if (!listed.at(index))
                    worldIds.push_back(index);
        for (const auto i : worldIds)
            listed[i] = true;

        std::size_t recovered = 0, ambiguous = 0, unreferenced = 0;
        for (std::size_t i = 0; i < listed.size(); ++i)
        {
            if (listed[i])
                continue;
            ++unreferenced;
            if (world.brushOwnershipComplete)
                continue;
            const auto models = std::span(world.models).subspan(world.models.empty() ? 0 : 1);
            if (std::ranges::any_of(models, [&](const ClipModel& model) { return InsideModel(world.brushes[i], model); }))
                ++ambiguous;
            else
            {
                ++recovered;
                worldIds.push_back(i);
            }
        }

        auto records = Json::array();
        std::size_t dropped = 0;
        for (const auto index : worldIds)
        {
            const auto& brush = world.brushes[index];
            if (brush.contents && !(brush.contents & ~nonCollidingContents))
                ++dropped;
            else
                records.push_back(BrushRecord(brush, world.materials));
        }
        Json summary{{"world_brushes", records.size()}, {"entity_brushes_excluded", owned},
                     {"world_brushes_recovered_unlisted", recovered}, {"unlisted_brushes_ambiguous_skipped", ambiguous},
                     {"flag_only_brushes_dropped", dropped}, {"brush_ownership_complete", world.brushOwnershipComplete},
                     {"unreferenced_brushes", unreferenced}};
        return {std::move(records), std::move(summary)};
    }

    py::Json BuildSubmodels(const CollisionWorld& world)
    {
        auto records = Json::array();
        for (std::size_t i = 1; i < world.models.size(); ++i)
        {
            const auto& model = world.models[i];
            auto brushes = Json::array();
            for (const auto brush : model.brushes)
                brushes.push_back(BrushRecord(world.brushes.at(brush), world.materials));
            records.push_back({{"mins", model.mins}, {"maxs", model.maxs}, {"brushes", std::move(brushes)}});
        }
        return records;
    }
}

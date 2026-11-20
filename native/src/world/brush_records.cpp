#include "world/collision_records.h"

#include "core/files.h"
#include "world/geometry_math.h"

#include <algorithm>
#include <bit>
#include <span>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        using util::Json;

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

        RecordPoint Point(const ClipPoint& p)
        {
            return {p[0], p[1], p[2]};
        }

        BrushRecord MakeBrushRecord(const ClipBrush& brush, const std::vector<ClipMaterial>& materials)
        {
            BrushRecord record;
            auto& axial = record.axial;
            auto& sides = record.sides;
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
                    sides.push_back({Point(plane.normal), plane.distance, contents, flags});
            }
            if (brush.vertices.empty())
                record.verts = BrushVertices(brush);
            else
                for (const auto& v : brush.vertices)
                    record.verts.push_back(Point(v));
            record.mins = Point(brush.mins);
            record.maxs = Point(brush.maxs);
            record.contents = std::bit_cast<std::int32_t>(brush.contents);
            return record;
        }

        bool InsideModel(const ClipBrush& brush, const ClipModel& model)
        {
            for (std::size_t axis = 0; axis < brush.mins.size(); ++axis)
                if (!(model.mins[axis] - 1.0 <= brush.mins[axis] && brush.maxs[axis] <= model.maxs[axis] + 1.0))
                    return false;
            return true;
        }
    }

    Json BrushRecord::ToJson() const
    {
        auto sidesJson = Json::array();
        for (const auto& side : sides)
            sidesJson.push_back({side.normal[0], side.normal[1], side.normal[2], side.distance, side.contents, side.surfaceFlags});
        return {{"mins", mins}, {"maxs", maxs}, {"contents", contents}, {"axial", axial}, {"sides", std::move(sidesJson)}, {"verts", verts}};
    }

    BrushRecord BrushRecord::FromJson(const Json& brush)
    {
        BrushRecord record;
        record.mins = brush.at("mins").get<RecordPoint>();
        record.maxs = brush.at("maxs").get<RecordPoint>();
        record.contents = brush.at("contents").get<std::int32_t>();
        record.axial = brush.at("axial").get<std::vector<std::array<std::int32_t, 2>>>();
        for (const auto& side : brush.at("sides"))
            record.sides.push_back({{side.at(0).get<double>(), side.at(1).get<double>(), side.at(2).get<double>()}, side.at(3).get<double>(),
                                    side.at(4).get<std::int32_t>(), side.at(5).get<std::int32_t>()});
        record.verts = brush.at("verts").get<std::vector<RecordPoint>>();
        return record;
    }

    Json SubmodelRecord::ToJson() const
    {
        auto brushesJson = Json::array();
        for (const auto& brush : brushes)
            brushesJson.push_back(brush.ToJson());
        return {{"mins", mins}, {"maxs", maxs}, {"brushes", std::move(brushesJson)}};
    }

    SubmodelRecords ReadSubmodels(const std::filesystem::path& path)
    {
        SubmodelRecords out;
        for (const auto& model : Json::parse(files::ReadText(path)).at("submodels"))
        {
            auto& record = out.emplace_back();
            record.mins = model.at("mins").get<RecordPoint>();
            record.maxs = model.at("maxs").get<RecordPoint>();
            for (const auto& brush : model.at("brushes"))
                record.brushes.push_back(BrushRecord::FromJson(brush));
        }
        return out;
    }

    void WriteSubmodels(const std::filesystem::path& path, const SubmodelRecords& submodels)
    {
        auto list = Json::array();
        for (const auto& model : submodels)
            list.push_back(model.ToJson());
        files::WriteText(path, util::JsonDumps({{"submodels", std::move(list)}}, -1) + "\n");
    }

    BrushRecords BuildWorldBrushes(const CollisionWorld& world)
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

        std::vector<BrushRecord> records;
        std::size_t dropped = 0;
        for (const auto index : worldIds)
        {
            const auto& brush = world.brushes[index];
            if (brush.contents && !(brush.contents & ~nonCollidingContents))
                ++dropped;
            else
                records.push_back(MakeBrushRecord(brush, world.materials));
        }
        Json summary{{"world_brushes", records.size()}, {"entity_brushes_excluded", owned},
                     {"world_brushes_recovered_unlisted", recovered}, {"unlisted_brushes_ambiguous_skipped", ambiguous},
                     {"flag_only_brushes_dropped", dropped}, {"brush_ownership_complete", world.brushOwnershipComplete},
                     {"unreferenced_brushes", unreferenced}};
        return {std::move(records), std::move(summary)};
    }

    SubmodelRecords BuildSubmodels(const CollisionWorld& world)
    {
        SubmodelRecords records;
        for (std::size_t i = 1; i < world.models.size(); ++i)
        {
            const auto& model = world.models[i];
            auto& record = records.emplace_back();
            record.mins = Point(model.mins);
            record.maxs = Point(model.maxs);
            for (const auto brush : model.brushes)
                record.brushes.push_back(MakeBrushRecord(world.brushes.at(brush), world.materials));
        }
        return records;
    }
}

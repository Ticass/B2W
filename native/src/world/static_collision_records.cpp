#include "world/collision_records.h"

#include "world/geometry_math.h"

#include <bit>
#include <stdexcept>

namespace w2b::world
{
    namespace
    {
        struct Placement
        {
            Vec3 origin;
            std::array<Vec3, 3> inverse;

            explicit Placement(const ClipStaticModel& model) : origin(ToVec3(model.origin))
            {
                const auto& a = model.inverseScaledAxis;
                const std::array<Vec3, 3> rows{{{a[0], a[3], a[6]}, {a[1], a[4], a[7]}, {a[2], a[5], a[8]}}};
                const std::array columns{Cross(rows[1], rows[2]), Cross(rows[2], rows[0]), Cross(rows[0], rows[1])};
                const auto det = Dot(rows[0], columns[0]);
                if (std::abs(det) < 1e-12)
                    throw std::runtime_error("degenerate static model transform");
                for (std::size_t i = 0; i < inverse.size(); ++i)
                    inverse[i] = {columns[0][i] / det, columns[1][i] / det, columns[2][i] / det};
            }

            Vec3 Apply(const Vec3& v) const
            {
                return {origin[0] + Dot(inverse[0], v), origin[1] + Dot(inverse[1], v), origin[2] + Dot(inverse[2], v)};
            }
        };

        bool Outside(const ClipStaticModel& model, const Placement& placement, const std::array<float, 12>& tri)
        {
            const std::array<Vec3, 3> rows{{{tri[0], tri[1], tri[2]}, {tri[4], tri[5], tri[6]}, {tri[8], tri[9], tri[10]}}};
            for (const auto& uv : {std::array{0.0, 0.0}, std::array{1.0, 0.0}, std::array{0.0, 1.0}})
            {
                const auto point = Solve3(rows, {tri[3], uv[0] + tri[7], uv[1] + tri[11]});
                if (!point)
                    return false;
                const auto placed = placement.Apply(*point);
                for (std::size_t i = 0; i < placed.size(); ++i)
                    if (!(model.absmin[i] - 2.0 <= placed[i] && placed[i] <= model.absmax[i] + 2.0))
                        return true;
            }
            return false;
        }
    }

    py::Json StaticModelRecord::ToJson() const
    {
        return {{"name", name}, {"contents", contents}, {"origin", origin}, {"invScaledAxis", invScaledAxis}, {"absmin", absmin}, {"absmax", absmax}};
    }

    StaticModelRecords BuildStaticModels(const CollisionWorld& world)
    {
        std::vector<StaticModelRecord> records;
        std::size_t outside = 0, triangles = 0, noCollision = 0;
        for (const auto& model : world.staticModels)
        {
            if (!model.contents || model.surfaces.empty())
            {
                ++noCollision;
                continue;
            }
            const Placement placement(model);
            for (const auto& surface : model.surfaces)
            {
                triangles += surface.triangles.size();
                for (const auto& triangle : surface.triangles)
                    outside += Outside(model, placement, triangle);
            }
            auto& record = records.emplace_back();
            record.name = model.name;
            record.contents = std::bit_cast<std::int32_t>(model.contents);
            std::ranges::copy(model.origin, record.origin.begin());
            std::ranges::copy(model.inverseScaledAxis, record.invScaledAxis.begin());
            std::ranges::copy(model.absmin, record.absmin.begin());
            std::ranges::copy(model.absmax, record.absmax.begin());
        }
        py::Json summary{{"static_models", records.size()}, {"static_model_collision_triangles", triangles},
                         {"static_models_without_collision", noCollision},
                         {"static_model_triangles_outside_waw_bounds", outside}};
        return {std::move(records), std::move(summary)};
    }
}

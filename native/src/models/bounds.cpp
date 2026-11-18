#include "models/models.h"

#include "core/sum.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace w2b::models
{
    namespace
    {
        constexpr auto Infinity = std::numeric_limits<double>::infinity();

        Json Negate(const Json& value)
        {
            constexpr auto Maximum = std::numeric_limits<std::int64_t>::max();
            if (value.is_number_unsigned())
            {
                const auto n = value.get<std::uint64_t>();
                if (n <= static_cast<std::uint64_t>(Maximum))
                    return -static_cast<std::int64_t>(n);
            }
            else if (value.is_number_integer())
            {
                const auto n = value.get<std::int64_t>();
                if (n != std::numeric_limits<std::int64_t>::min())
                    return -n;
                return static_cast<std::uint64_t>(Maximum) + 1;
            }
            return -value.get<double>();
        }

        void Extend(Bounds& bounds, const Json& accessor)
        {
            const auto& lo = accessor.at("min");
            const auto& hi = accessor.at("max");
            std::array mins{lo.at(0), Negate(hi.at(2)), lo.at(1)};
            std::array maxs{hi.at(0), Negate(lo.at(2)), hi.at(1)};
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                if (mins[axis].get<double>() < bounds.mins[axis].get<double>())
                    bounds.mins[axis] = std::move(mins[axis]);
                if (maxs[axis].get<double>() > bounds.maxs[axis].get<double>())
                    bounds.maxs[axis] = std::move(maxs[axis]);
            }
        }

        Vec3 Sub(const Vec3& a, const Vec3& b)
        {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }

        Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        double Dot(const Vec3& a, const Vec3& b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        Json CollisionTriangle(const Vec3& a, const Vec3& b, const Vec3& c)
        {
            const auto e1 = Sub(b, a), e2 = Sub(c, a);
            auto normal = Cross(e2, e1);
            const auto length = std::sqrt(Dot(normal, normal));
            if (length == 0)
                throw std::invalid_argument("degenerate collision triangle");
            for (auto& component : normal)
                component /= length;
            const auto c12 = Cross(e2, normal), c20 = Cross(normal, e1), c01 = Cross(e1, e2);
            const auto determinant = Dot(e1, c12);
            if (std::abs(determinant) < 1e-12)
                throw std::invalid_argument("degenerate collision triangle");
            Vec3 s{}, t{};
            for (std::size_t i = 0; i < 3; ++i)
            {
                s[i] = (1.0 * c12[i] + 0.0 * c20[i] + 0.0 * c01[i]) / determinant;
                t[i] = (0.0 * c12[i] + 1.0 * c20[i] + 0.0 * c01[i]) / determinant;
            }
            const auto plane = [&a](const Vec3& vector) {
                return Json::array({vector[0], vector[1], vector[2], Dot(vector, a)});
            };
            return {{"plane", plane(normal)}, {"svec", plane(s)}, {"tvec", plane(t)}};
        }
    }

    std::optional<Bounds> GameBounds(const Json& geometry)
    {
        Bounds bounds{{Infinity, Infinity, Infinity}, {-Infinity, -Infinity, -Infinity}};
        const auto meshes = geometry.find("meshes");
        if (meshes == geometry.end())
            return std::nullopt;
        for (const auto& mesh : *meshes)
            if (const auto primitives = mesh.find("primitives"); primitives != mesh.end())
                for (const auto& primitive : *primitives)
                    Extend(bounds, geometry.at("accessors").at(primitive.at("attributes").at("POSITION").get<std::size_t>()));
        if (bounds.mins[0].get<double>() == Infinity)
            return std::nullopt;
        return bounds;
    }

    Vec3 LightingOrigin(const Json& placement, const Bounds& bounds)
    {
        const auto axis = placement.at("axis").get<std::array<double, 9>>();
        const auto origin = placement.at("origin").get<Vec3>();
        const auto scale = placement.at("scale").get<double>();
        const std::array<Vec3, 2> limit{
            Vec3{bounds.mins[0].get<double>(), bounds.mins[1].get<double>(), bounds.mins[2].get<double>()},
            Vec3{bounds.maxs[0].get<double>(), bounds.maxs[1].get<double>(), bounds.maxs[2].get<double>()},
        };
        Vec3 lo{Infinity, Infinity, Infinity}, hi{-Infinity, -Infinity, -Infinity};
        for (unsigned corner = 0; corner < 8; ++corner)
            for (unsigned k = 0; k < 3; ++k)
            {
                Vec3 terms{};
                for (unsigned r = 0; r < 3; ++r)
                    terms[r] = limit[(corner >> r) & 1][r] * axis[r * 3 + k];
                const auto point = origin[k] + scale * py::Sum(terms);
                lo[k] = std::min(lo[k], point);
                hi[k] = std::max(hi[k], point);
            }
        return {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, hi[2] * 0.75 + lo[2] * 0.25};
    }

    Json CollisionBox(Bounds bounds, const Json& bone)
    {
        Vec3 lo{}, hi{}, center{};
        for (std::size_t k = 0; k < 3; ++k)
        {
            lo[k] = bounds.mins[k].get<double>();
            hi[k] = bounds.maxs[k].get<double>();
            center[k] = (lo[k] + hi[k]) * 0.5;
            if (hi[k] - lo[k] < 1.0)
            {
                lo[k] = center[k] - 0.5;
                hi[k] = center[k] + 0.5;
                bounds.mins[k] = lo[k];
                bounds.maxs[k] = hi[k];
                center[k] = (lo[k] + hi[k]) * 0.5;
            }
        }
        const auto corner = [&](int i) -> Vec3 {
            return {i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]};
        };
        constexpr std::array<std::array<int, 4>, 6> faces{{
            {0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6},
        }};
        auto triangles = Json::array();
        for (const auto& face : faces)
            for (std::size_t i : {1, 2})
            {
                const auto a = corner(face[0]);
                auto b = corner(face[i]), c = corner(face[i + 1]);
                if (Dot(Cross(Sub(c, a), Sub(b, a)), Sub(a, center)) < 0)
                    std::swap(b, c);
                triangles.push_back(CollisionTriangle(a, b, c));
            }
        return {{"tris", std::move(triangles)}, {"mins", std::move(bounds.mins)}, {"maxs", std::move(bounds.maxs)},
                {"bone", bone}, {"contents", ScriptModelContents}, {"surfFlags", 0}};
    }
}

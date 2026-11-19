#include "mapents/map_entity.h"
#include "physics/constraints.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <locale>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>

namespace w2b::physics
{
    namespace
    {
        using Axis = std::array<Vector, 3>;
        using Names = std::set<std::string>;

        struct Endpoint
        {
            Vector position;
            std::optional<std::string> parent;
        };

        Axis QuaternionAxis(const std::array<double, 4>& q)
        {
            const auto [x, y, z, w] = q;
            return {{{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                     {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                     {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}}};
        }

        Axis EulerAxis(const Vector& angles)
        {
            const auto [pitch, yaw, roll] = angles;
            constexpr auto Degrees = std::numbers::pi / 180.0;
            const auto sp = std::sin(pitch * Degrees), cp = std::cos(pitch * Degrees);
            const auto sy = std::sin(yaw * Degrees), cy = std::cos(yaw * Degrees);
            const auto sr = std::sin(roll * Degrees), cr = std::cos(roll * Degrees);
            return {{{cp * cy, cp * sy, -sp}, {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp},
                     {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp}}};
        }

        Vector Transform(const Vector& point, const Vector& origin, const Axis& axis)
        {
            auto result = origin;
            for (std::size_t i = 0; i < result.size(); ++i)
                result[i] += point[0] * axis[0][i] + point[1] * axis[1][i] + point[2] * axis[2][i];
            return result;
        }

        Vector ParseVector(const std::string& source)
        {
            std::istringstream input(source);
            input.imbue(std::locale::classic());
            Vector result{};
            std::string extra;
            if (!(input >> result[0] >> result[1] >> result[2]) || (input >> extra))
                throw std::invalid_argument("invalid rope attachment vector: " + source);
            return result;
        }

        const mapents::MapEntity& FindEntity(const mapents::MapEntities& entities, const std::string& name)
        {
            const mapents::MapEntity* match = nullptr;
            for (const auto& entity : entities)
            {
                if (entity.Get("targetname") != name)
                    continue;
                const auto type = entity.Get("classname");
                if (type != "script_model" && type != "script_brushmodel")
                    continue;
                if (match)
                    throw std::invalid_argument("native rope attachment " + name + ": expected one script model");
                match = &entity;
            }
            if (!match)
                throw std::invalid_argument("native rope attachment " + name + ": expected one script model");
            return *match;
        }

        Endpoint ResolveEndpoint(const Json& joint, int index, const mapents::MapEntities& entities, const Json& dynents)
        {
            const auto point = JointPosition(joint, index).get<Vector>();
            const auto attachment = joint.at(JointField("attach_point_type", index)).get<int>();
            switch (attachment)
            {
            case 0:
                return {point, std::nullopt};
            case 1:
            {
                const auto target = joint.at(JointField("target_index", index)).get<std::size_t>();
                const auto& body = dynents.at(target);
                const auto& name = weapons::Field(body, "targetname");
                if (name.empty())
                    throw std::invalid_argument("native rope dynent attachment " + std::to_string(target) + ": no targetname");
                return {Transform(point, body.at("origin").get<Vector>(), QuaternionAxis(body.at("quat").get<std::array<double, 4>>())), name};
            }
            case 2:
            {
                const auto& name = weapons::Field(joint, JointField("target_ent", index));
                const auto& entity = FindEntity(entities, name);
                const auto* origin = entity.Find("origin");
                if (!origin)
                    throw std::invalid_argument("native rope attachment " + name + ": entity has no origin");
                return {Transform(point, ParseVector(*origin), EulerAxis(ParseVector(std::string(entity.Get("angles", "0 0 0"))))), name};
            }
            default:
                throw std::invalid_argument("native rope entity translation requires world or entity attachment");
            }
        }

        std::string DescribeJoint(const Json& joint, std::size_t index)
        {
            std::string targets;
            for (int endpoint : {1, 2})
            {
                if (joint.at(JointField("attach_point_type", endpoint)) == 0)
                    continue;
                if (!targets.empty())
                    targets += ", ";
                const auto& name = weapons::Field(joint, JointField("target_ent", endpoint));
                targets += name.empty() ? "dynent " + py::JsonDumps(joint.at(JointField("target_index", endpoint)), -1) : name;
            }
            return std::format("{} joint {} ({})", JointTypes.at(joint.at("type").get<std::size_t>()), index,
                               targets.empty() ? "world" : targets);
        }

        Json MakeRope(const Json& joint, const Vector& anchor)
        {
            return {{"classname", "rope"}, {"origin", FormatVector(anchor, " ")},
                    {"length", py::JsonDumps(joint.at("distance"), -1)}, {"force_scale", py::JsonDumps(joint.at("scale").at(0), -1)},
                    {"length_scale", py::JsonDumps(joint.at("scale").at(1), -1)}, {"width", py::JsonDumps(joint.at("minAngle"), -1)},
                    {"damp", py::JsonDumps(joint.at("damp"), -1)}, {"spawnflags", py::JsonDumps(joint.at("flags"), -1)},
                    {"timeout", py::FloatRepr(joint.at("timeout").get<double>() / 1000.0)}};
        }

        void AddMaterial(const Path& project, const Json& joint, Json& rope, Names& materials)
        {
            const auto& material = weapons::Field(joint, "material");
            if (material.empty())
                return;
            weapons::OutputName("material", material);
            const auto alias = "mc/waw_rope/" + material;
            weapons::CopySource(project / assets::MaterialPath(material), project / assets::MaterialPath(alias));
            rope["material"] = alias.substr(3);
            materials.insert(alias);
        }

        void AddEndpoints(const std::array<Endpoint, 2>& endpoints, const Vector& anchor, std::size_t index,
                          Json& rope, Json& generated, Names& names)
        {
            for (std::size_t i = 0; i < endpoints.size(); ++i)
            {
                const auto& [point, parent] = endpoints[i];
                if (!parent && point == anchor)
                    continue;
                const auto name = std::format("waw_rope_{:04}_end{}", index, i + 1);
                if (!names.insert(name).second)
                    throw std::invalid_argument("native rope endpoint name collision: " + name);
                Json endpoint{{"classname", "info_null"}, {"targetname", name}, {"origin", FormatVector(point, " ")}};
                if (parent && !parent->empty())
                    endpoint["target"] = *parent;
                generated.push_back(std::move(endpoint));
                rope[i == 0 ? "target" : "target2"] = name;
            }
        }
    }

    Json StageRopeEntities(const Path& project)
    {
        const auto manifest = project / ConstraintManifest;
        const auto empty = []() -> Json { return {{"ropes", 0}, {"endpoints", 0}, {"materials", Json::array()}}; };
        if (!std::filesystem::is_regular_file(manifest))
            return empty();
        const auto joints = weapons::ReadAssetJson(manifest).at("constraints");
        if (joints.empty())
            return empty();
        const auto dynentFile = project / "BSP/dynents.json";
        const auto dynents = std::filesystem::is_regular_file(dynentFile) ? weapons::ReadAssetJson(dynentFile).at("dynents") : Json::array();
        const auto path = project / "BSP/entities.json";
        auto entities = mapents::ReadMapEntities(path);
        auto generated = Json::array();
        auto unsupported = Json::array();
        Names materials, names;
        for (const auto& entity : entities)
            names.insert(std::string(entity.Get("targetname")));
        for (std::size_t i = 0; i < joints.size(); ++i)
        {
            const auto& joint = joints.at(i);
            if (joint.at("type") != RopeJoint)
            {
                unsupported.push_back(DescribeJoint(joint, i));
                continue;
            }
            const std::array endpoints{ResolveEndpoint(joint, 1, entities, dynents), ResolveEndpoint(joint, 2, entities, dynents)};
            const auto& origin = endpoints[0].parent && !endpoints[1].parent ? endpoints[1].position : endpoints[0].position;
            auto rope = MakeRope(joint, origin);
            if (const auto& name = weapons::Field(joint, "targetname"); !name.empty())
                rope["targetname"] = name;
            AddMaterial(project, joint, rope, materials);
            AddEndpoints(endpoints, origin, i, rope, generated, names);
            generated.push_back(std::move(rope));
        }
        const auto ropes = joints.size() - unsupported.size();
        const auto endpointCount = generated.size() - ropes;
        std::erase_if(entities, [](const mapents::MapEntity& entity) { return entity.Get("classname") == "rope"; });
        for (const auto& entity : generated)
            entities.push_back(mapents::MapEntityFromJson(entity));
        mapents::WriteMapEntities(path, entities);
        return {{"ropes", ropes}, {"endpoints", endpointCount}, {"materials", materials}, {"unsupported_joints", std::move(unsupported)}};
    }
}

#include "physics/constraints.h"

#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

namespace w2b::physics
{
    namespace
    {
        bool Finite(const Json& value)
        {
            return value.is_number() && std::isfinite(value.get<double>());
        }

        void StageAttachment(Json& joint, int endpoint, std::size_t dynentCount)
        {
            const auto attachment = joint.at(JointField("attach_point_type", endpoint)).get<int>();
            if (attachment < 0 || attachment > 3)
                throw std::invalid_argument("unknown WaW physics attachment type");
            if (attachment == 1)
            {
                const auto target = joint.at(JointField("target_index", endpoint)).get<std::int64_t>();
                if (target < 0 || static_cast<std::uint64_t>(target) >= dynentCount)
                    throw std::invalid_argument("map physics dynent attachment outside the map dynents");
            }
            if (attachment == 2 || attachment == 3)
            {
                if (weapons::Field(joint, JointField("target_ent", endpoint)).empty())
                    throw std::invalid_argument("map physics entity attachment has no name to rebind");
                joint[JointField("target_index", endpoint)] = -1;
            }
        }
    }

    std::string JointField(std::string_view field, int endpoint)
    {
        return std::string(field) + std::to_string(endpoint);
    }

    const Json& JointPosition(const Json& joint, int endpoint)
    {
        return joint.at(endpoint == 1 ? "pos" : "pos2");
    }

    std::string FormatNumber(double value)
    {
        char text[64];
        const auto [end, error] = std::to_chars(std::begin(text), std::end(text), value, std::chars_format::general, 9);
        if (error != std::errc{})
            throw std::invalid_argument("cannot format map physics coordinate");
        return {text, end};
    }

    std::string FormatVector(const Vector& position, std::string_view separator)
    {
        return FormatNumber(position[0]) + std::string(separator) + FormatNumber(position[1]) +
               std::string(separator) + FormatNumber(position[2]);
    }

    Json StageConstraints(const Path& source, const Path& project, std::size_t dynentCount)
    {
        const auto target = project / ConstraintManifest;
        if (!std::filesystem::is_regular_file(source))
        {
            std::filesystem::remove(target);
            return {{"count", 0}, {"ropes", 0}, {"materials", Json::array()}, {"exported", false}};
        }
        auto data = weapons::ReadAssetJson(source);
        if (data.value("version", 0) != 1)
            throw std::invalid_argument("unsupported map physics constraint schema");
        auto& joints = data.at("constraints");
        if (joints.size() > 511)
            throw std::invalid_argument("map physics constraint count exceeds T6 limit 511");
        std::set<std::string> materials;
        std::size_t ropes = 0;
        for (auto& joint : joints)
        {
            const auto type = joint.at("type").get<int>();
            if (type < 0 || type >= static_cast<int>(JointTypes.size()))
                throw std::invalid_argument("unknown WaW physics constraint type");
            ropes += type == RopeJoint;
            for (const auto endpoint : {1, 2})
                StageAttachment(joint, endpoint, dynentCount);
            for (const auto* field : {"offset", "pos", "pos2", "dir", "scale"})
            {
                const auto& vector = joint.at(field);
                if (!vector.is_array() || vector.size() != 3 || !std::ranges::all_of(vector, Finite))
                    throw std::invalid_argument("invalid map physics vector " + std::string(field));
            }
            for (const auto* field : {"distance", "damp", "power", "spin_scale", "minAngle", "maxAngle"})
                if (!Finite(joint.at(field)))
                    throw std::invalid_argument("invalid map physics scalar " + std::string(field));
            auto& material = joint.at("material").get_ref<std::string&>();
            material.erase(0, material.find_first_not_of(','));
            if (!material.empty())
                materials.insert(material);
        }
        if (ropes > 300)
            throw std::invalid_argument("map rope count exceeds T6 limit 300");
        files::WriteText(target, py::JsonDumps(data, 2) + "\n");
        return {{"count", joints.size()}, {"ropes", ropes}, {"materials", materials}, {"exported", true}};
    }
}

#include "barriers/barriers.h"

#include "barriers/floor.h"
#include "core/files.h"
#include "mapents/entities.h"
#include "physics/constraints.h"

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>
#include <numbers>
#include <set>
#include <sstream>

namespace w2b::barriers
{
    namespace
    {
        using mapents::Field;
        constexpr std::string_view NativeBoard = "zbarrier_zmcore_BasicWoodBarrier";
        constexpr std::string_view Tear = "ai_zombie_boardtear_aligned_m_1_grab";

        bool IsGoal(const Json& entity)
        {
            return Field(entity, "targetname") == "exterior_goal" && !Field(entity, "target").empty();
        }

        Path Animation(const Path& bo2, std::string_view name)
        {
            return bo2 / "raw/xanim" / files::FromUtf8(name);
        }

        Vec3 Forward(std::string_view angles)
        {
            std::istringstream input{std::string(angles)};
            input.imbue(std::locale::classic());
            double pitch{}, yaw{}, roll{};
            if (!(input >> pitch >> yaw >> roll))
                throw Error("invalid barricade angles: " + std::string(angles));
            constexpr auto radians = std::numbers::pi / 180;
            pitch *= radians;
            yaw *= radians;
            return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
        }

        double BoardDepth(const Json& board, const Vec3& origin, const Vec3& forward)
        {
            if (!board.contains("origin"))
                throw Error("source barricade board has no origin");
            const auto point = mapents::Origin(board);
            Vec3 factors{};
            for (std::size_t i = 0; i < factors.size(); ++i)
                factors[i] = (point[i] - origin[i]) * forward[i];
            return Sum(factors);
        }

        double RepairDepth(const Json& native, const Path& bo2, Json& poses)
        {
            long long count{};
            bool overflow{};
            if (!py::ParseInt(Field(native, "zbarriernumboards"), count, overflow) || overflow || count <= 0)
                throw Error("invalid barricade board count");
            std::vector<double> depths;
            for (long long i = 1; i <= count; ++i)
            {
                const auto& name = native.at("zbarrierboardanim" + std::to_string(i)).get_ref<const std::string&>();
                if (!poses.contains(name))
                    poses[name] = RepairedTranslation(Animation(bo2, name));
                depths.push_back(poses.at(name).at(0).get<double>());
            }
            return Sum(depths) / static_cast<double>(depths.size());
        }

        struct Window
        {
            std::vector<std::size_t> native;
            std::vector<std::size_t> boards;
        };

        std::map<std::string, Window, std::less<>> Windows(const Json& entities)
        {
            std::map<std::string, Window, std::less<>> result;
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entity = entities.at(i);
                const auto name = Field(entity, "targetname");
                if (name.empty())
                    continue;
                const auto type = Field(entity, "classname");
                if (type == NativeBoard)
                    result[std::string(name)].native.push_back(i);
                if (type == "script_brushmodel" && Field(entity, "script_noteworthy") != "clip")
                    result[std::string(name)].boards.push_back(i);
            }
            return result;
        }
    }

    Json AlignBoards(Json& entities, const Path& bo2, const world::CollisionWorld* clip)
    {
        std::optional<Floor> floor;
        const auto height = clip ? -RepairedTranslation(Animation(bo2, Tear))[2] : 0;
        const auto windows = Windows(entities);
        auto poses = Json::object(), aligned = Json::array(), warnings = Json::array();
        for (const auto& goal : entities)
        {
            if (!IsGoal(goal))
                continue;
            const auto target = Field(goal, "target");
            const auto group = windows.find(target);
            if (group == windows.end() || group->second.native.size() != 1 || group->second.boards.empty())
                continue;
            const auto origin = mapents::Origin(goal);
            const auto angles = std::string(Field(goal, "angles", "0 0 0"));
            const auto forward = Forward(angles);
            std::vector<double> depths;
            for (const auto index : group->second.boards)
                depths.push_back(BoardDepth(entities.at(index), origin, forward));
            auto& native = entities.at(group->second.native.front());
            const auto sourceDepth = Sum(depths) / static_cast<double>(depths.size());
            const auto nativeDepth = RepairDepth(native, bo2, poses);
            const auto shift = sourceDepth - nativeDepth;
            Vec3 root{};
            for (std::size_t i = 0; i < root.size(); ++i)
                root[i] = origin[i] + forward[i] * shift;
            Json item{{"target", target}, {"source_depth", sourceDepth}, {"native_repaired_depth", nativeDepth},
                      {"root_advance", shift}};
            if (clip)
            {
                if (!floor)
                    floor.emplace(*clip);
                const auto ground = floor->Below(root[0], root[1], origin[2] + 8);
                if (!ground || *ground < origin[2] - 64)
                    warnings.push_back(std::string(target) + " at " + std::string(Field(goal, "origin", "None")) +
                                       ": no floor under the barrier; height left at the WaW goal");
                else
                {
                    item["floor"] = *ground;
                    item["lift"] = *ground + height - root[2];
                    root[2] = *ground + height;
                }
            }
            native["origin"] = physics::FormatVector(root, " ");
            native["angles"] = angles;
            aligned.push_back(std::move(item));
        }
        return {{"aligned", std::move(aligned)}, {"repair_poses", std::move(poses)}, {"warnings", std::move(warnings)}};
    }

    Json DropChunks(Json& entities)
    {
        std::map<std::string, std::size_t, std::less<>> natives;
        for (const auto& entity : entities)
            if (Field(entity, "classname") == NativeBoard)
                ++natives[std::string(Field(entity, "targetname"))];
        std::set<std::string, std::less<>> replaced;
        for (const auto& goal : entities)
        {
            if (!IsGoal(goal))
                continue;
            const auto target = Field(goal, "target");
            const auto group = natives.find(target);
            if (group != natives.end() && group->second == 1)
                replaced.emplace(target);
        }
        if (replaced.empty())
            return {{"windows", 0}, {"dropped_brushes", 0}};
        const auto before = entities.size();
        const auto last = std::remove_if(entities.begin(), entities.end(), [&](const Json& entity) {
            return Field(entity, "classname") == "script_brushmodel" && replaced.contains(Field(entity, "targetname"));
        });
        entities.erase(last, entities.end());
        return {{"windows", replaced.size()}, {"dropped_brushes", before - entities.size()}};
    }
}

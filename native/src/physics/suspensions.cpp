#include "physics/constraints.h"

#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <map>

namespace w2b::physics
{
    namespace
    {
        constexpr std::string_view Begin = "    // BEGIN SCRIPT SUSPENSIONS";
        constexpr std::string_view End = "    // END SCRIPT SUSPENSIONS";

        bool CanSuspend(const Json& joint, int opposite, std::size_t jointCount)
        {
            if (jointCount != 1 || joint.at("type") != RopeJoint || joint.at(JointField("attach_point_type", opposite)) != 0 ||
                joint.at("distance").get<double>() <= 0)
                return false;
            for (const auto* key : {"flags", "timeout", "min_health", "max_health"})
                if (joint.value(key, 0.0) != 0)
                    return false;
            return true;
        }

        std::string ScriptVector(const Json& vector)
        {
            return "(" + FormatVector(vector.get<Vector>(), ", ") + ")";
        }

        std::string MakeBlock(const Json& joints, const Json& entities, Json& unsupported, std::size_t& count)
        {
            std::map<std::string, std::vector<const Json*>> owners;
            for (const auto& joint : joints)
                for (const auto endpoint : {1, 2})
                    if (joint.at(JointField("attach_point_type", endpoint)) == 2)
                        owners[weapons::Field(joint, JointField("target_ent", endpoint))].push_back(&joint);

            auto block = std::string(Begin) + "\n    level.waw_rope_suspensions = [];\n";
            for (const auto& [name, entries] : owners)
            {
                const auto& joint = *entries.front();
                const auto endpoint = weapons::Field(joint, "target_ent1") == name ? 1 : 2;
                const auto opposite = 3 - endpoint;
                const auto hasModel = [&] {
                    return std::ranges::any_of(entities, [&name](const Json& entity) {
                        return weapons::Field(entity, "targetname") == name && weapons::Field(entity, "classname") == "script_model";
                    });
                };
                if (!CanSuspend(joint, opposite, entries.size()) || !hasModel())
                {
                    unsupported.push_back(name);
                    continue;
                }
                block += "    suspension = spawnstruct();\n";
                block += "    suspension.anchor = " + ScriptVector(JointPosition(joint, opposite)) + ";\n";
                block += "    suspension.point = " + ScriptVector(JointPosition(joint, endpoint)) + ";\n";
                block += "    suspension.length = " + FormatNumber(joint.at("distance").get<double>()) + ";\n";
                block += "    suspension.damp = " + FormatNumber(joint.at("damp").get<double>()) + ";\n";
                block += "    level.waw_rope_suspensions[" + py::JsonDumps(name, -1) + "] = suspension;\n";
                ++count;
            }
            block += End;
            return block;
        }

        void InstallBlock(std::string& source, const std::string& block)
        {
            std::size_t search = 0;
            for (auto start = source.find(Begin); start != source.npos; start = source.find(Begin, search))
            {
                const auto end = source.find(End, start + Begin.size());
                if (end == source.npos)
                    break;
                source.replace(start, end + End.size() - start, block);
                search = start + block.size();
            }
            if (search != 0)
                return;
            constexpr std::string_view Initializer = "init()\n{";
            const auto start = source.find(Initializer);
            if (start == source.npos)
                throw std::invalid_argument("asset initializer missing while registering rope suspensions");
            source.insert(start + Initializer.size(), "\n" + block);
        }
    }

    Json StageSuspensions(const Path& project)
    {
        const auto assets = project / "maps/mp/waw/_waw2bo2_assets.gsc";
        auto unsupported = Json::array();
        if (!std::filesystem::is_regular_file(assets))
            return {{"suspensions", 0}, {"unsupported", std::move(unsupported)}};
        const auto manifest = project / ConstraintManifest;
        const auto joints = std::filesystem::is_regular_file(manifest) ? weapons::ReadAssetJson(manifest).at("constraints") : Json::array();
        const auto entities = joints.empty() ? Json::array() : weapons::ReadAssetJson(project / "BSP/entities.json").at("entities");
        auto source = files::ReadText(assets);
        std::size_t count = 0;
        InstallBlock(source, MakeBlock(joints, entities, unsupported, count));
        files::WriteText(assets, source);
        return {{"suspensions", count}, {"unsupported", std::move(unsupported)}};
    }
}

#include "physics/physics.h"

#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::physics
{
    Json StageDynents(const Path& project, const Json& entries, const std::vector<Path>& roots,
                      const PresetLookup& stock, std::string_view effectPrefix)
    {
        std::set<std::string> names;
        for (const auto& entity : entries)
            if (auto name = PresetName(entity); !name.empty())
                names.insert(std::move(name));
        auto staged = Json::object();
        auto presets = Json::object();
        std::vector<std::string> converted;
        for (const auto& name : names)
        {
            auto inputs = roots;
            if (stock)
                if (const auto root = stock(name))
                    inputs.push_back(*root);
            auto output = weapons::StagePhysicsPreset(inputs, project, name, staged);
            presets[name] = output;
            converted.push_back(std::move(output));
        }
        std::sort(converted.begin(), converted.end());
        auto report = WriteDynents(project, entries, presets, effectPrefix);
        report["physpresets"] = std::move(converted);
        return report;
    }

    Json StageRopeRuntime(const Path& project)
    {
        auto report = StageRopeEntities(project);
        report["script_suspensions"] = StageSuspensions(project);
        return report;
    }

    Json LoadMapPhysics(const Path& dynentSource, const Path& constraintSource, const Path& project)
    {
        auto dynents = ReadDynents(dynentSource);
        auto constraints = StageConstraints(constraintSource, project, dynents.size());
        return {{"dynents", std::move(dynents)}, {"constraints", std::move(constraints)}};
    }
}

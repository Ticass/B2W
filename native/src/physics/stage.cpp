#include "physics/physics.h"

#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <map>
#include <set>

namespace w2b::physics
{
    namespace
    {
        Path ReadPath(const Json& request, std::string_view key)
        {
            return files::FromUtf8(request.at(key).get_ref<const std::string&>());
        }

        Json StageDynentsRequest(const Json& request)
        {
            const auto project = ReadPath(request, "project");
            const auto& entries = request.at("entries");
            const auto& fallback = request.at("stockRoots");
            std::vector<Path> roots;
            for (const auto& root : request.at("roots"))
                roots.push_back(files::FromUtf8(root.get_ref<const std::string&>()));
            const auto stock = [&](const std::string& name) -> std::optional<Path> {
                return fallback.contains(name) ? std::optional{ReadPath(fallback, name)} : std::nullopt;
            };
            return StageDynents(project, entries, roots, stock, request.at("effectPrefix").get_ref<const std::string&>());
        }
    }

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

    Json Run(const Path& file)
    {
        using Operation = Json (*)(const Json&);
        static const std::map<std::string_view, Operation, std::less<>> operations{
            {"load-map", [](const Json& request) {
                return LoadMapPhysics(ReadPath(request, "dynents"), ReadPath(request, "constraints"), ReadPath(request, "project"));
            }},
            {"read-dynents", [](const Json& request) { return ReadDynents(ReadPath(request, "source")); }},
            {"write-dynents", [](const Json& request) {
                return WriteDynents(ReadPath(request, "project"), request.at("entries"), request.at("presets"),
                                    request.at("effectPrefix").get_ref<const std::string&>());
            }},
            {"stage-dynents", StageDynentsRequest},
            {"stage-constraints", [](const Json& request) {
                return StageConstraints(ReadPath(request, "source"), ReadPath(request, "project"), request.at("dynentCount").get<std::size_t>());
            }},
            {"rope-entities", [](const Json& request) { return StageRopeEntities(ReadPath(request, "project")); }},
            {"rope-suspensions", [](const Json& request) { return StageSuspensions(ReadPath(request, "project")); }},
            {"rope-runtime", [](const Json& request) { return StageRopeRuntime(ReadPath(request, "project")); }},
        };
        try
        {
            const auto request = weapons::ReadAssetJson(file);
            const auto operation = operations.find(request.at("operation").get_ref<const std::string&>());
            if (operation == operations.end())
                throw std::invalid_argument("unknown map physics operation");
            return {{"result", operation->second(request)}};
        }
        catch (const weapons::WeaponError& error)
        {
            return {{"weaponError", error.what()}};
        }
        catch (const std::invalid_argument& error)
        {
            return {{"invalid", error.what()}};
        }
        catch (const Json::exception& error)
        {
            return {{"invalid", error.what()}};
        }
    }
}

#include "weapons/stage.h"

#include "core/files.h"
#include "weapons/plan.h"

#include <algorithm>
#include <map>
#include <regex>

namespace w2b::weapons
{
    namespace
    {
        Names SelectWeapons(const Plan& plan, const std::vector<std::string>& requested)
        {
            std::map<std::string_view, const Weapon*> byName;
            for (std::size_t i = 0; i < plan.sources.size(); ++i)
                byName.emplace(plan.sources[i].name, &plan.weapons[i]);
            Names selected;
            auto pending = requested;
            Names missing;
            for (const auto& name : pending)
                if (!byName.contains(name))
                    missing.insert(name);
            if (!missing.empty())
                throw WeaponError("compiled weapons not found: " + NameList(missing));
            while (!pending.empty())
            {
                auto name = std::move(pending.back());
                pending.pop_back();
                if (!selected.insert(name).second)
                    continue;
                const auto weapon = byName.find(name);
                if (weapon == byName.end())
                    throw WeaponError("alternate weapon not found: " + name);
                const auto alternatives = GetDependencies(weapon->second->dependencies, "weapon");
                pending.insert(pending.end(), alternatives.begin(), alternatives.end());
            }
            return selected;
        }

        void StageAccuracy(const std::vector<path>& roots, const path& project, const Json& deps, Json& report)
        {
            auto staged = Json::array(), missing = Json::array();
            for (const std::string_view folder : {"aivsai", "aivsplayer"})
                for (const auto& name : GetDependencies(deps, "accuracy_" + std::string(folder)))
                {
                    const auto renamed = OutputName("accuracy", name);
                    const auto parent = "accuracy" / files::FromUtf8(folder);
                    const auto relative = parent / files::FromUtf8(name);
                    const auto source = FindSource(roots, relative);
                    if (source.empty())
                    {
                        missing.push_back(PosixPath(relative));
                        continue;
                    }
                    CopySource(source, project / parent / files::FromUtf8(renamed));
                    staged.push_back({{"name", renamed}, {"folder", folder}, {"source", files::Utf8(source)}});
                }
            report["accuracy_graphs"] = std::move(staged);
            report["missing_accuracy_graphs"] = std::move(missing);
        }

        std::string StageAnimations(const std::vector<path>& roots, const path& project, const Json& deps, Json& report)
        {
            auto staged = Json::array(), missing = Json::array();
            std::string zone;
            for (const auto& name : GetDependencies(deps, "xanim"))
            {
                const auto renamed = OutputName("xanim", name);
                const auto source = FindSource(roots, "xanim" / files::FromUtf8(name));
                if (source.empty())
                {
                    missing.push_back(name);
                    continue;
                }
                CopySource(source, project / "xanim" / files::FromUtf8(renamed));
                staged.push_back({{"source", files::Utf8(source)}, {"name", renamed}});
                zone += "xanim," + renamed + "\n";
            }
            report["animations"] = std::move(staged);
            report["missing_animations"] = std::move(missing);
            return zone;
        }
    }

    WeaponStage StageWeapons(const std::vector<path>& roots, const path& project, const path& base, const std::vector<std::string>& requested)
    {
        auto plan = PlanWeapons(roots, project, base);
        const auto selected = SelectWeapons(plan, requested);
        auto dependencies = Json::object();
        Names outputNames, worldModels;
        std::vector<std::string> zoneWeapons;
        static const std::regex worldModel(R"(^worldModel([2-9]|1[0-6])?$)");
        for (std::size_t i = 0; i < plan.weapons.size(); ++i)
        {
            if (!selected.contains(plan.sources[i].name))
                continue;
            const auto& weapon = plan.weapons[i];
            outputNames.insert(weapon.name);
            zoneWeapons.push_back(weapon.name);
            for (const auto& [kind, names] : weapon.dependencies.items())
                for (const auto& asset : names)
                    AddDependency(dependencies, kind, asset.get_ref<const std::string&>());
            for (const auto& [key, value] : weapon.fields.items())
                if (key.starts_with("worldModel") && std::regex_match(key, worldModel))
                    worldModels.insert(value.get_ref<const std::string&>());
            files::WriteText(project / "weapons" / files::FromUtf8(weapon.name), WriteInfo(weapon.fields));
        }
        auto& report = plan.report;
        auto weapons = Json::array();
        for (auto& entry : report.at("weapons"))
            if (outputNames.contains(entry.at("name").get_ref<const std::string&>()))
                weapons.push_back(std::move(entry));
        report["weapons"] = std::move(weapons);
        report["status"] = "partial_source_stage";
        report["flame_tables"] = StageFlameTables(roots, project, dependencies,
            base / "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/FlameTableFields.h");
        auto zone = StageAnimations(roots, project, dependencies, report);
        StageAccuracy(roots, project, dependencies, report);
        std::ranges::sort(zoneWeapons);
        for (const auto& name : zoneWeapons)
            zone += "weapon," + name + "\n";
        files::WriteText(project / "weapons.zone.fragment", zone.empty() ? "\n" : zone);
        for (auto& names : dependencies)
            names = names.get<Names>();
        report["dependencies"] = std::move(dependencies);
        report["remaining"] = {"model/material conversion", "sound-bank conversion", "FX dependency linking",
            "unsupported weapon field translation", "Zombies gameplay registration", "native roundtrip and in-game validation"};
        return {std::move(report), std::move(worldModels)};
    }
}

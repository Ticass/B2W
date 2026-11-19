#pragma once

#include "weapons/weapon.h"

#include <set>

namespace w2b::weapons
{
    using Names = std::set<std::string>;

    std::string PosixPath(const path& value);
    std::string NameList(const Names& values);
    path FindSource(const std::vector<path>& roots, const path& relative);
    path FindModelLod(const std::vector<path>& roots, const path& owner, const std::string& file);
    void CopySource(const path& source, const path& destination);
    Json ReadAssetJson(const path& source);
    std::string StagePhysicsPreset(const std::vector<path>& roots, const path& project, const std::string& name, Json& report);
    void AddDependency(Json& dependencyLists, std::string_view kind, const std::string& value);
    Names GetDependencies(const Json& dependencyLists, std::string_view kind);
    Json StageFlameTables(const std::vector<path>& roots, const path& project, Json& dependencies, const path& schema);
    struct WeaponStage
    {
        Json report;       // content_source/weapons.stage.json before its models section
        Names worldModels; // the staged weapons' world models
    };

    // The compiled WaW weapons the map uses, with their dependency closure. Throws WeaponError.
    WeaponStage StageWeapons(const std::vector<path>& roots, const path& project, const path& base, const std::vector<std::string>& names);
    // The weapons' view and world models; returns the report's models section. Throws WeaponError.
    Json StageModels(const std::vector<path>& roots, const path& project, const Names& names, const Names& worldModels);
    Json StageShadowVariants(const path& content);
}

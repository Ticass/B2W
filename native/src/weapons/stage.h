#pragma once

#include "weapons/weapon.h"

#include <set>

namespace w2b::weapons
{
    using Names = std::set<std::string>;

    std::string PosixPath(const path& value);
    std::string NameList(const Names& values);
    path FindSource(const std::vector<path>& roots, const path& relative);
    void CopySource(const path& source, const path& destination);
    Json ReadAssetJson(const path& source);
    void AddDependency(Json& dependencyLists, std::string_view kind, const std::string& value);
    Names GetDependencies(const Json& dependencyLists, std::string_view kind);
    Json StageFlameTables(const std::vector<path>& roots, const path& project, Json& dependencies, const path& schema);
    Json StageWeapons(const path& request);
    Json StageModels(const path& request);
    Json StageShadowVariants(const path& content);
}

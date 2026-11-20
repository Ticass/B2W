#pragma once

#include "core/text.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace w2b::physics
{
    using Path = std::filesystem::path;
    using util::Json;
    using PresetLookup = std::function<std::optional<Path>(const std::string&)>;

    Json ReadDynents(const Path& source);
    std::string PresetName(const Json& entity);
    Json WriteDynents(const Path& project, const Json& entries, const Json& presets, std::string_view effectPrefix);
    Json StageDynents(const Path& project, const Json& entries, const std::vector<Path>& roots,
                      const PresetLookup& stock, std::string_view effectPrefix);
    Json StageConstraints(const Path& source, const Path& project, std::size_t dynentCount);
    Json LoadMapPhysics(const Path& dynentSource, const Path& constraintSource, const Path& project);
    Json StageRopeEntities(const Path& project);
    Json StageSuspensions(const Path& project);
    Json StageRopeRuntime(const Path& project);
}

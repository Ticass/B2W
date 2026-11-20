#pragma once

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <set>

namespace w2b::world
{
    struct RenderWorld;
    struct Visibility;

    util::Json WriteStagedWorld(RenderWorld& world, const std::filesystem::path& fbx,
                             const std::set<std::string>& blendMaterials, const std::set<std::string>& shadowMaterials,
                             const std::set<std::string>& ownLightCasters, Visibility* visibility);
    util::Json ExportWorldFbx(const std::filesystem::path& source, const std::filesystem::path& output,
                           std::optional<std::size_t> surfaceLimit);
}

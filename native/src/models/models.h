#pragma once

#include "core/text.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>

namespace w2b::models
{
    using util::Json;
    using Path = std::filesystem::path;
    using Vec3 = std::array<double, 3>;

    struct Bounds
    {
        std::array<Json, 3> mins, maxs;
    };

    constexpr int ScriptModelContents = 0x2080;

    std::optional<Bounds> GameBounds(const Json& geometry);
    Vec3 LightingOrigin(const Json& placement, const Bounds& bounds);
    Json CollisionBox(Bounds bounds, const Json& bone);
    bool StripBadSkins(Json& geometry);
    std::size_t NeutralizeTransforms(Json& geometry);
    std::vector<std::string> DropShadowSurfaces(Json& geometry, const std::function<bool(const std::string&)>& shadowOnly);
    Json StageModels(const Path& project, const Path& partCsv, std::vector<Path> roots, Json placements,
                     std::set<std::string, std::less<>> extraModels, std::set<std::string, std::less<>> boxModels);

    // a converted WaW model an effect spawns, under a name of its own
    struct EffectModelAsset
    {
        std::string sourceModel, name, file;
        std::vector<std::string> lods;
    };

    struct EffectModelAssets
    {
        std::vector<EffectModelAsset> entries;
        std::vector<std::string> errors;
        Json EntriesJson() const;
    };

    EffectModelAssets StageEffectAssets(const Path& project, const std::set<std::string>& names);
}

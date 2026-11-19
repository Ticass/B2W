#pragma once
// weapons.stage_visuals: the weapons' materials through the measured
// material translator (world/model ones via stage_materials, effect ones on
// stock BO2 effect donors with their shader runtime), their original images
// (resolved through the IWD recovery that serves this operation) and the
// technique sets and shader programs they use; content_source/weapons.visuals.json.

#include "core/pytext.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    struct WeaponVisualsRequest
    {
        std::vector<std::filesystem::path> roots;
        std::filesystem::path project; // the project's content_source
        std::filesystem::path stockMaterials, techsetDump;
        std::optional<std::filesystem::path> bo2Raw;
        std::map<std::string, std::filesystem::path> nativeFallbacks; // material -> BO2 equivalent
        bool layeredVertexData = false;
    };

    struct WeaponVisuals
    {
        std::vector<std::string> warnings;
        std::size_t errors = 0;
        py::Json recovered; // IWD images decoded for the weapons
    };

    // writes content_source/weapons.visuals.json
    WeaponVisuals StageWeaponVisuals(const WeaponVisualsRequest& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline

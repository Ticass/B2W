#pragma once
// WaW assets read as objects from zones loaded in this process: no dump, no
// file, no JSON. Serialized with every other use of OpenAssetTools.

#include "materials/waw_material.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::oat
{
    // a WaW zone and the search paths the Unlinker's --search-path takes
    struct WawZone
    {
        std::filesystem::path fastfile;
        std::vector<std::filesystem::path> searchPaths;
    };

    // the material the zone defines under `name` (case-insensitive); nullopt
    // when the zone only references it or lacks it
    std::optional<materials::WawMaterial> ReadMaterial(const WawZone& zone, const std::string& name);
    // the names of the materials the zone defines, in zone order
    std::vector<std::string> MaterialNames(const WawZone& zone);
} // namespace w2b::oat

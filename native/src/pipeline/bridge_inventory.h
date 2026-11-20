#pragma once

#include "core/text.h"
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    struct Inventory // what the map's zones and roots ship
    {
        std::set<std::string> compiledWeapons, zoneSounds, shippedModels;
    };

    Inventory BridgeInventory(const std::vector<std::filesystem::path>& roots, const std::vector<std::filesystem::path>& mapRoots,
                              const std::optional<std::filesystem::path>& entities, const util::Json& levelWeapons, std::vector<std::string>& warnings);
}

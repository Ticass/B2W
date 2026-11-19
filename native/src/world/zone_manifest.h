#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::world
{
    // The map zone's assets, written in this order.
    struct MapZoneAssets
    {
        std::vector<std::string> image, xmodel, zbarrier, fx, material, script, localize, rawfile;
    };

    std::filesystem::path WriteMapZone(const std::filesystem::path& stage, const std::string& project, bool ipak,
                                       const MapZoneAssets& assets);
}

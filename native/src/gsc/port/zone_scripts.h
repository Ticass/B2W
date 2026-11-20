#pragma once

#include "core/text.h"

#include <filesystem>
#include <set>

namespace w2b::gsc::port
{
    struct ZoneGraph
    {
        std::vector<std::string> initial;
        std::vector<std::string> adjacency;

        std::set<std::string> Names() const;
        util::Json ToJson() const;
    };

    struct ZoneMainResult
    {
        std::string error;
        bool guardedSwitch = false;
    };

    ZoneGraph ReadZoneGraph(std::string_view source);
    ZoneGraph ReadZoneGraphFile(const std::filesystem::path& source);
    ZoneMainResult StageZoneMain(const std::filesystem::path& main, bool dropManager, bool guardSwitch);
}

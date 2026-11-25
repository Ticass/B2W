#pragma once
// Assets of the map's zones that nothing in the converter reads, so that none
// is left out silently.

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    // a warning per asset type of these zones the converter has no reader for, and per script menu it does not convert
    std::vector<std::string> UnreadAssets(const std::filesystem::path& project, const std::vector<std::filesystem::path>& roots);

    // sounds the author's own linker did not find: the log it stores in the zone names them
    std::set<std::string> SoundsLinkerMissed(const std::vector<std::filesystem::path>& roots);
} // namespace w2b::pipeline

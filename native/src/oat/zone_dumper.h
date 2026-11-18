#pragma once
// Named assets dumped from WaW zones kept loaded in this process: a build
// asks stock zones for a few hundred assets one at a time, and dumping each
// from an already loaded zone takes milliseconds where a whole-kind dump of
// the zone takes minutes.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace w2b::oat
{
    struct AssetDumpRun
    {
        bool ok = false;
        std::string log; // OAT's messages, as the Unlinker prints them
    };

    // Dumps `assets` ((kind, name) pairs, names compared without case) of the
    // zone `fastfile` into `output` (DDS images, glTF models), with the search
    // paths the Unlinker's --search-path takes. The zone stays loaded for later
    // calls while it fits the memory budget. Serialized with RunUnlinker.
    AssetDumpRun DumpAssets(const std::filesystem::path& fastfile, const std::vector<std::filesystem::path>& searchPaths,
                            const std::vector<std::pair<std::string, std::string>>& assets, const std::filesystem::path& output);
} // namespace w2b::oat

#pragma once

#include <cstdint>

#include <filesystem>
#include <vector>

namespace w2b::files
{
    struct SharedFiles
    {
        std::size_t files = 0;
        std::uintmax_t bytes = 0;
    };

    // hard-links each staged file (relative to staging) to an identical copy in a prior folder
    SharedFiles ShareIdenticalFiles(const std::filesystem::path& staging, const std::vector<std::filesystem::path>& prior,
                                    const std::vector<std::filesystem::path>& names);
}

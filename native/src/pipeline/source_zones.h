#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace w2b::pipeline
{
    // the fastfile a dump root was read from
    std::optional<std::filesystem::path> Fastfile(const std::filesystem::path& root);
    // the fastfiles of these roots, each once
    std::vector<std::filesystem::path> SourceZones(const std::vector<std::filesystem::path>& roots);
} // namespace w2b::pipeline

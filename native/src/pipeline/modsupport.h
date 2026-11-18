#pragma once
// Small build-mod steps: ipakfix.py (move ipak blocks off streaming window
// ends) and overheathud.py (the stock overheat HUD with its typos repaired).

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    // the inserted padding sizes; empty for a readable ipak
    std::vector<unsigned> RepairIpak(const std::filesystem::path& path);
    std::string RepairOverheatBytecode(std::string data);
    // the zone lines the overheat HUD adds (none when no weapon overheats)
    std::vector<std::string> StageOverheatHud(const std::filesystem::path& bo2, const std::filesystem::path& project, const std::filesystem::path& work,
                                              const std::filesystem::path& unlinker);
} // namespace w2b::pipeline

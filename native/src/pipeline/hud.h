#pragma once
// Port of wawhud.stage_runtime (build-mod): WaW's engine-owned HUD through T6
// LUI, the stock HUD modules wrapped and the HUD's string tables.

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    // csv.writer(lineterminator="\n").writerows, written as Path.write_text writes it
    void WriteTable(const std::filesystem::path& path, const std::vector<std::vector<std::string>>& rows);
    // the mod zone lines of the converted HUD (none when staging made no HUD)
    std::vector<std::string> StageHudRuntime(const std::filesystem::path& bo2, const std::filesystem::path& project, const std::string& name);
} // namespace w2b::pipeline

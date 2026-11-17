#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::sound
{
    struct StageInput
    {
        std::vector<std::string> roots;   // str(Path) of each source root
        std::vector<std::string> iwdDirs; // folders of .iwd files, or .iwd files
        std::optional<std::set<std::string>> names; // None: every alias
        std::optional<std::string> globalsFile;     // resolved by the caller (stock fallback included)
        std::filesystem::path project;
    };

    // sounds.stage: alias IR files, original audio payloads and sounds.stage.json
    py::Json Stage(const StageInput& input);
} // namespace w2b::sound

#pragma once
// The build steps the launcher tracks from the worker's "== N." lines.

#include <string>

namespace w2b::ui
{
    inline constexpr const char* STEPS[] = {"Extract source map", "Prepare BO2 references", "Convert map assets", "Build gameplay & menus",
                                            "Compile map scripts", "Link converted map",    "Verify materials",   "Assemble map package"};
    inline constexpr int STEP_COUNT = 8;

    inline int StageIndex(const std::string& stage)
    {
        static const std::pair<const char*, int> table[] = {{"0", 0}, {"1", 1}, {"1b", 1}, {"1c", 1}, {"2", 2}, {"3a", 3}, {"3b", 4}, {"4", 5}, {"6", 6}, {"7", 7}};
        for (const auto& [name, index] : table)
            if (stage == name)
                return index;
        return -1;
    }
} // namespace w2b::ui

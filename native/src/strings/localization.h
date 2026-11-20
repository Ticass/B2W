#pragma once
// The map's localized UI text from its zones, IWDs,
// stock WaW and raw WaW, isolated from BO2's string keys (WAW2BO2_ prefix).

#include "core/text.h"

#include <filesystem>
#include <map>
#include <optional>
#include <vector>
#include <string>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::strings
{
    // StringEd text -> key -> value (the last definition of a repeated key);
    // conflicts, when given, receive the repeated keys whose text differs
    std::map<std::string, std::string> ParseStringEd(const std::string& text, const std::string& language, util::Json* conflicts = nullptr);
    std::string Quote(const std::string& text);
    std::string Unquote(const std::string& text);

    struct LocalizationRequest
    {
        std::filesystem::path project;
        std::vector<std::filesystem::path> roots, iwdDirs, rawRoots;
        std::optional<std::filesystem::path> unlinker, work, bo2Root, t6Unlinker, t6StockDump;
        std::string language = "english";
        const extract::StockWaw* stock = nullptr;
    };

    // localization.stage: the summary for the bridge report
    util::Json StageLocalization(const LocalizationRequest& request);
} // namespace w2b::strings

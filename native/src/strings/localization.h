#pragma once
// Port of localization.py: the map's localized UI text from its zones, IWDs,
// stock WaW and raw WaW, isolated from BO2's string keys (WAW2BO2_ prefix).

#include "core/pytext.h"

#include <filesystem>
#include <map>
#include <string>

namespace w2b::strings
{
    // StringEd text -> key -> value (the last definition of a repeated key);
    // conflicts, when given, receive the repeated keys whose text differs
    std::map<std::string, std::string> ParseStringEd(const std::string& text, const std::string& language, py::Json* conflicts = nullptr);
    std::string Quote(const std::string& text);
    std::string Unquote(const std::string& text);

    // localization.stage; request fields as the Python call's arguments
    py::Json StageLocalization(const std::filesystem::path& request);
} // namespace w2b::strings

#pragma once
// Checks translated scripts as T6 loads them, before anything is compiled:
// every call and function pointer resolves, with a number of arguments T6
// takes. T6 reports these one at a time, at map load, as "Unresolved external".

#include "t6api.h"

#include <filesystem>
#include <string>
#include <vector>

namespace w2b::gsc::port
{
    // Server (.gsc) and client (.csc) scripts under `root`, one line per
    // problem: "maps\mp\waw\_x:120 (function f): ...". `prefixes`: only scripts
    // whose path starts with one of them are checked; all of them resolve calls.
    std::vector<std::string> LinkCheck(const std::filesystem::path& root, const T6Api& api, const std::vector<std::string>& prefixes = {});
} // namespace w2b::gsc::port

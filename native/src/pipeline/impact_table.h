#pragma once
// WaW's impact effects: the table that names the effect of every hit type on
// every surface, and the effects WaW's engine itself plays. BO2 looks its own
// up the same way, so the map zone carries the map's.

#include "core/text.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    struct ImpactTable
    {
        std::vector<std::vector<std::string>> rows; // WaW's: 16 hit types, 31 surfaces and 4 flesh hits; empty: none found
        std::string zone;                           // the fastfile it is in
        std::set<std::string> effects;              // what it and the engine name
    };

    // the table the game would find: the last loaded zone's, so the map's zones, else common.ff's
    ImpactTable ReadImpacts(const std::vector<std::filesystem::path>& roots, const extract::StockWaw* stock);

    // BO2's rows of a WaW table, through the converted names ("" where the effect did not convert)
    std::vector<std::vector<std::string>> T6Impacts(const std::vector<std::vector<std::string>>& rows, const std::map<std::string, std::string>& converted);

    struct StagedImpacts
    {
        std::vector<std::string> table, fx; // zone entries
        std::set<std::string> missing;      // effects without a conversion
    };
    StagedImpacts StageImpacts(const std::filesystem::path& project, const ImpactTable& impacts, const std::map<std::string, std::string>& converted);
} // namespace w2b::pipeline

#pragma once
// framework_coverage: what runs in place of each WaW zombie-framework
// function of the map (ported, bo2, compat, not run), written beside the
// report as framework_coverage.json/.md, with a FRAMEWORK_BO2 warning for
// each function BO2 runs without a documented glue reason.

#include "core/text.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace w2b::pipeline
{
    struct CoverageSummary
    {
        std::vector<std::pair<std::string, long long>> totals; // status -> functions
        std::vector<std::string> warnings;                     // FRAMEWORK_BO2 rows without a glue reason
        util::Json TotalsJson() const
        {
            auto json = util::Json::object();
            for (const auto& [status, count] : totals)
                json[status] = count;
            return json;
        }
    };

    // scripts: the bridge report's scripts section
    CoverageSummary FrameworkCoverage(const std::string& project, const std::filesystem::path& projectRoot,
                                      const std::vector<std::filesystem::path>& roots, const util::Json& scripts);
} // namespace w2b::pipeline

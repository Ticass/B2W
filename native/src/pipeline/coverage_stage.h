#pragma once
// framework_coverage: what runs in place of each WaW zombie-framework
// function of the map (ported, bo2, compat, not run), written beside the
// report as framework_coverage.json/.md, with a FRAMEWORK_BO2 warning for
// each function BO2 runs without a documented glue reason.

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project, project_root, roots, scripts (the report's)};
    // returns {totals, warnings}
    py::Json FrameworkCoverage(const std::filesystem::path& request);
} // namespace w2b::pipeline

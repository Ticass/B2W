#pragma once
// t6bridge.port_scripts (see scripts_stage.cpp).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request: the port_scripts arguments; returns the report additions
    // ({scripts, warnings, errors}), the models, the widened model roots and
    // the stock WaW dumps prepared on the way
    py::Json PortScripts(const std::filesystem::path& request);
    // gscport.link_check over the completed graph: request {project_root, api_cache}; returns {problems}
    py::Json LinkCheckStep(const std::filesystem::path& request);
} // namespace w2b::pipeline

#pragma once
// t6bridge.stage_bridge, the client scripts: the environment script (native
// flame pools for carried gas weapons, the tesla client), the BO2 client
// main's hooks and the map's own client main (clientport.finish_map_client).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, project, roots, fx_table, sound_names, characters,
    // map_client}; returns {warnings, map_client} or {value_error}
    py::Json FinishClientScripts(const std::filesystem::path& request);
} // namespace w2b::pipeline

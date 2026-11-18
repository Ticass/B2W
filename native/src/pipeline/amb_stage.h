#pragma once
// t6bridge.write_amb_csc: the map's clientscripts/mp/<map>_amb.csc, the
// WaW ambient rooms, packages and music state table ported to BO2's API
// (sounds.port_ambient / port_music / radverb_for).

#include "core/pytext.h"

#include <filesystem>
#include <set>
#include <string>

namespace w2b::pipeline
{
    // request {project, project_root, source_amb, ents}; returns {warnings, content}
    py::Json WriteAmbCsc(const py::Json& request);
    py::Json WriteAmbCscRequest(const std::filesystem::path& request);
    // sounds.ambient_aliases: what the source ambient packages and room tones play
    std::set<std::string> AmbientAliases(const std::string& source);
} // namespace w2b::pipeline

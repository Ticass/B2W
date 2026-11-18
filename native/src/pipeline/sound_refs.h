#pragma once
// t6bridge.stage_bridge, the sounds the map plays: script and entity alias
// references (sounds.script_aliases / entity_aliases), their dependency graph
// (assets::Expand, in process with the stock dumps) and the converted names
// bound into the entities and script comparisons
// (sounds.bind_entity_sounds / bind_script_sound_comparisons).

#include "core/pytext.h"

#include <filesystem>

namespace w2b::pipeline
{
    // request {project_root, roots, stock_waw, zone_sounds, compiled_sounds,
    // ents, source_amb}; returns {script_sounds, entity_sounds, compiled_sounds,
    // sound_graph, stock_roots, content, stock_prepared}
    py::Json DiscoverSounds(const py::Json& request);
    py::Json DiscoverSoundsRequest(const std::filesystem::path& request);
} // namespace w2b::pipeline

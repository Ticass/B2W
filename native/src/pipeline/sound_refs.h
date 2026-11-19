#pragma once
// t6bridge.stage_bridge, the sounds the map plays: script and entity alias
// references (sounds.script_aliases / entity_aliases), their dependency graph
// (assets::Expand, in process with the stock dumps) and the converted names
// bound into the entities and script comparisons
// (sounds.bind_entity_sounds / bind_script_sound_comparisons).

#include "core/pytext.h"
#include "pipeline/step_report.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    struct SoundDiscoveryRequest
    {
        std::filesystem::path projectRoot;
        std::vector<std::filesystem::path> roots;
        extract::StockWaw* stock = nullptr;
        std::vector<std::string> zoneSounds, compiledSounds;
        std::optional<std::filesystem::path> ents, sourceAmb; // the map's entities, its clientscripts/<map>_amb.csc
    };

    struct SoundDiscovery
    {
        std::vector<std::string> scriptSounds, entitySounds, soundNames;
        py::Json soundGraph;                                   // content_source/sounds.dependencies.json
        std::vector<std::pair<std::string, std::filesystem::path>> stockRoots; // zone -> dump the closure used
        py::Json animationSoundIdentities;
        StepReport report;                                     // the sound dependency content sections
    };

    SoundDiscovery DiscoverSounds(const SoundDiscoveryRequest& request);
} // namespace w2b::pipeline

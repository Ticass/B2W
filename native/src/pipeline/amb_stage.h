#pragma once
// t6bridge.write_amb_csc: the map's clientscripts/mp/<map>_amb.csc, the
// WaW ambient rooms, packages and music state table ported to BO2's API
// (sounds.port_ambient / port_music / radverb_for).

#include "core/text.h"
#include "pipeline/step_report.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>

namespace w2b::pipeline
{
    // the map's ambient client script (<project>_amb.csc) from its WaW one, the entities and the driver's reverb
    StepReport WriteAmbCsc(const std::string& project, const std::filesystem::path& projectRoot,
                           const std::optional<std::filesystem::path>& sourceAmb, const std::optional<std::filesystem::path>& ents);
    // sounds.ambient_aliases: what the source ambient packages and room tones play
    std::set<std::string> AmbientAliases(const std::string& source);
} // namespace w2b::pipeline

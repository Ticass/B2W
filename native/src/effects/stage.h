#pragma once

#include "core/text.h"
#include "effects/effect.h"

#include <filesystem>
#include <map>
#include <set>
#include <utility>
#include <string>
#include <vector>

namespace w2b::effects
{
    struct EffectRequest
    {
        std::filesystem::path output; // the project root; effects go to fx/
        std::vector<std::pair<std::string, std::filesystem::path>> effects; // name -> dumped WaW effect
        std::set<std::string> absent;                                       // effects in no WaW source
        MaterialNames materialNames;
        std::set<std::string> failedMaterials;
        unsigned workers = 1;
    };

    struct EffectConversion
    {
        std::set<std::string> converted;
        std::size_t failed = 0;
        std::vector<std::string> warnings;
        std::set<std::string> models, sounds; // what the converted effects spawn and play
    };

    EffectConversion StageEffects(const EffectRequest& request);

    // an effect variant without the stationary model a script spawns itself
    struct ModelOverlay
    {
        std::string source, model, variant;
    };
    util::Json ToJson(const std::vector<ModelOverlay>& overlays);

    // effects: script-loaded WaW effect -> converted name
    std::vector<ModelOverlay> StageModelOverlayRuntime(const std::filesystem::path& project, const std::map<std::string, std::string>& effects);
}

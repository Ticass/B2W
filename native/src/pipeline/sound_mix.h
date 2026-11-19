#pragma once
// t6bridge.stage_bridge, the sound mix: the stock WaW and BO2 measurements
// (audioremaster stock profiles and classes, bus groups, reverb send, the
// loaded-bank budget) and the bank written with them (sound/bank.cpp).

#include "core/pytext.h"
#include "pipeline/step_report.h"
#include "sound/bank.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    struct SoundMixRequest
    {
        std::filesystem::path projectRoot;
        std::string project;
        std::optional<std::filesystem::path> bo2Root;
        extract::StockWaw* stock = nullptr;
        std::vector<std::string> scriptSounds, entitySounds; // the sounds the map plays
        std::optional<std::filesystem::path> sourceAmb;
        std::string flacEncoder;
    };

    struct SoundMixResult
    {
        StepReport report;
        py::Json bankReport; // content_source/sounds.bank.json
        std::optional<sound::SoundBudget> budget; // the loaded-bank budget, none without BO2
    };

    SoundMixResult SoundMix(const SoundMixRequest& request);
} // namespace w2b::pipeline

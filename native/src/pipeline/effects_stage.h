#pragma once
// t6bridge.stage_effects: the effects the map's scripts load, their closure,
// effect materials, world-technique materials and the converted effects.

#include "core/text.h"
#include "pipeline/material_record.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
    class WawSource;
}

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::pipeline
{
    struct EffectsStageRequest
    {
        std::vector<std::string> names; // the effects the map's scripts load
        std::vector<std::filesystem::path> roots;
        std::filesystem::path projectRoot, stockMaterials, bo2Root;
        std::optional<std::filesystem::path> techsetDump;
        std::vector<std::filesystem::path> iwdDirs;
        extract::StockWaw* stock = nullptr;
        extract::WawSource* source = nullptr; // --waw-source-fx
        bool layeredVertexData = false;
        unsigned workers = 1;
    };

    // what the effects pass adds to the report and hands on (EffectsResult)
    struct EffectsStage
    {
        std::vector<std::string> warnings, errors;
        std::vector<MaterialRecord> materials;
        util::Json images;                           // report image records
        std::map<std::string, std::string> table;  // script-loaded WaW effect -> converted name
        std::vector<std::string> zoneFx;
        std::set<std::string> models, sounds;      // what the converted effects spawn and play
        std::vector<std::filesystem::path> extraRoots; // stock and source dumps the closure used
        std::set<std::string> techsets;
        util::Json recovered;                        // IWD images recovery decoded for it
    };

    EffectsStage StageEffectsStep(const EffectsStageRequest& request, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline

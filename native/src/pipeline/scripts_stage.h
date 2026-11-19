#pragma once
// t6bridge.port_scripts (see scripts_stage.cpp).

#include "core/pytext.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::extract
{
    class StockWaw;
    class WawSource;
}

namespace w2b::pipeline
{
    struct ScriptPortRequest
    {
        std::filesystem::path stage, projectRoot, mapScript, bo2Root;
        // script roots in load order; nullopt marks the map zone's own place (the patch zone loads after it)
        std::vector<std::optional<std::filesystem::path>> roots;
        std::vector<std::filesystem::path> iwdDirs, modelRoots, stockVariantRoots;
        std::optional<std::filesystem::path> stockScripts;
        extract::StockWaw* stock = nullptr;
        extract::WawSource* source = nullptr; // --waw-source-fx: the WaW Mod Tools sources
        bool stockPerks = false, stockMap = false;
    };

    struct ScriptPort
    {
        py::Json scripts; // the report's scripts section; empty when nothing ported
        std::vector<std::string> warnings, errors;
        std::vector<std::string> models;                // script models the map zone carries
        std::vector<std::filesystem::path> modelRoots;  // widened by the stock dumps the models needed
    };

    // port_scripts: the map's WaW scripts ported, with their animations and models
    ScriptPort PortScripts(const ScriptPortRequest& request);
    // gscport.link_check over the completed graph: request {project_root, api_cache}; returns {problems}
} // namespace w2b::pipeline

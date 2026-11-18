#pragma once
// cli stage-bridge and t6bridge.stage_bridge: stage everything the T6 bridge
// links for one map (geometry, scripts, weapons, effects, models, materials,
// images, then the finishing passes of bridge_tail.cpp), in this process.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    // the stage-bridge command line (cli.py's parser), paths resolved
    struct BridgeStageOptions
    {
        fs::path stage;
        std::string project;
        fs::path gfx, clip, stockMaterials;
        std::optional<fs::path> wawMapScript, techsetDump, bo2, scriptTemplate, t6Unlinker, wawRoot, t4Unlinker, wawStockDumps,
            audioDecoder, xwmaDecoder, wawModTools, wawSourceDumps, wawStockScripts, t6SoundDriver;
        std::optional<std::string> scriptTemplateName;
        std::optional<std::vector<fs::path>> iwdDirs;
        std::vector<fs::path> extraRoots, mapRoots, wawScriptRoots, wawPatchScriptRoots;
        bool noIpak = false, fxFallback = false, wawSourceFx = false, approximateSoundCurves = false, bo2StockPerks = false,
             wawStockMap = false, remasterBo2Materials = false;

        // throws StepError("ArgumentError") for an unknown or incomplete argument
        static BridgeStageOptions Parse(const std::vector<std::string>& args);
        // the opt-in staging that still runs only in Python (--waw-source-fx,
        // --fx-fallback, --remaster-bo2-materials); empty when this port covers it
        std::string PythonOnlyOption() const;
    };

    // stages the map and prints the stage-bridge summary; returns 1 when the
    // stage report has errors (cli._stage_bridge), throws StepError on failure
    int StageBridge(const BridgeStageOptions& options);
} // namespace w2b::pipeline

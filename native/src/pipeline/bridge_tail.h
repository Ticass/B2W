#pragma once
// The end of the bridge stage: map scripts and hooks, weapon runtime, visuals,
// client scripts, sounds, lighting, the window barrier, identities, menus,
// localization, rumbles, the map zone and the framework coverage.

#include "core/pytext.h"
#include "gsc/port/staging.h"
#include "gsc/port/zone_scripts.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::textures
{
    class IwdRecovery;
}

namespace w2b::extract
{
    class StockWaw;
}

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    struct TailScripts
    {
        std::set<std::string> usedTechsets;
        fs::path wawMapScript;
        std::optional<gsc::port::ScriptTemplate> requiredTemplate;
        std::optional<bool> dropZoneManager;          // set when the navigation pass found the map's zone graph
        std::optional<gsc::port::ZoneGraph> zoneGraph;
    };

    struct TailWeapons
    {
        bool staged = false; // the weapons pass staged the map's weapons
        fs::path stockMaterials;
        std::map<std::string, fs::path> nativeFallbacks;
        bool layeredVertexData = false;
        std::set<std::string> convertedFx;
        std::vector<fs::path> tableRoots, mapTableRoots;
    };

    struct TailSounds
    {
        std::optional<std::string> mapStem;
        std::vector<fs::path> ambRoots;
        fs::path ents;
        std::vector<std::string> zoneSounds, compiledSounds;
        std::vector<fs::path> iwdDirs;
        std::optional<fs::path> audioDecoder, xwmaHelper, resampler, t6SoundDriver;
        bool approximateCurves = false;
        py::Json visionReport; // filled by the visuals block
    };

    struct TailLighting
    {
        fs::path base, clipSource; // base: the WaW world dumps' common stem
        bool shadowLights = false;
        py::Json scriptRenderDvars = py::Json::object(); // filled by the visuals block
    };

    // what the earlier bridge passes hand the tail
    struct TailInput
    {
        std::string project;
        fs::path projectRoot, stage;
        std::vector<fs::path> roots, rawRoots, iwdDirs, coverageRoots;
        std::optional<fs::path> bo2Root, techsetDump, t4Unlinker, t6Unlinker;
        std::optional<std::string> wawMapStem;
        std::vector<std::string> images, hudMaterials, zoneModels, zoneFx;
        bool ipak = true, bo2StockPerks = false, hasWater = false;
        TailScripts scripts;
        TailWeapons weapons;
        std::vector<fs::path> visionIwdDirs, visionRoots;
        TailSounds sounds;
        TailLighting lighting;
    };

    // Adds to the bridge report and writes bridge_stage.report.json; stock: the
    // stock WaW zones the earlier passes used, null without them. Returns the
    // IWD images recovery decoded on the way.
    py::Json BridgeTail(TailInput input, py::Json& report, extract::StockWaw* stock, textures::IwdRecovery& recovery);
} // namespace w2b::pipeline

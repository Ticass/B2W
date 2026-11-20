#pragma once
// cli build-mod: the gameplay zone (mod.ff) linked by
// the BO2 mod tools from the zm_test template, the converted map's extras and
// its frontend tables, then the frontend-only lobby zone (mod_load.ff).

#include "core/text.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::pipeline
{
    namespace fs = std::filesystem;

    std::string BuildModZone(const fs::path& templateZone, std::vector<std::string>& dropped);
    std::string OpenModIpakFirst(const std::string& zoneText, const std::vector<std::string>& extraLines);
    std::string PruneTemplateWeapons(const std::string& zone, const fs::path& project, const fs::path& bo2, std::vector<std::string>& dropped);
    std::set<std::string> UnavailableWeapons(const std::vector<std::string>& unavailable);
    std::set<std::string> LoadedWeapons(const fs::path& work);
    std::vector<std::string> PruneWeaponReferences(const fs::path& script, const std::set<std::string>& weapons);
    std::set<std::string> StockScripts(const fs::path& bo2, const fs::path& unlinker, const std::optional<fs::path>& stockDump);
    std::vector<std::string> ScriptClosure(const std::string& zoneText, const fs::path& raw, const std::set<std::string>& stock);
    std::vector<std::string> CharacterModels(const std::string& zoneText, const fs::path& raw);

    util::Json MenuMetadata(const fs::path& projectRoot);
    std::string MapNameKey(const std::string& project);
    std::vector<std::string> StageMenuAssets(const fs::path& projectRoot, const std::string& project);
    fs::path StageLobbyMapTable(const fs::path& stock, const fs::path& projectRoot, const std::string& project);
    fs::path StageLobbyGametypeTable(const fs::path& stock, const fs::path& projectRoot, const std::string& project);
    fs::path LinkLobby(const fs::path& bo2, const fs::path& projectRoot, const std::string& project, const fs::path& work, const fs::path& linker,
                       const std::optional<fs::path>& stock);
    void VerifyLobbyTables(const fs::path& ff, const fs::path& unlinker, const fs::path& work, const std::string& project);

    // (mod.ff, template entries dropped for lack of raw assets)
    std::pair<fs::path, std::vector<std::string>> LinkMod(const fs::path& bo2, const fs::path& work, const fs::path& unlinker, std::vector<std::string> extraLines,
                                                          const std::optional<fs::path>& assetRoot, const std::vector<fs::path>& extraAssetRoots,
                                                          const std::optional<fs::path>& linker, const std::optional<fs::path>& stockDump);
    void ActivateModShaders(const fs::path& ff, const fs::path& project, const fs::path& work, const fs::path& linker, const fs::path& unlinker, const fs::path& stock);

    struct BuildModOptions
    {
        fs::path stage, bo2, work, unlinker;
        std::string project;
        std::optional<fs::path> linker, techsetDump, scriptsReady;
    };
    // cli build-mod
    void BuildMod(const BuildModOptions& options);
} // namespace w2b::pipeline

#pragma once
// The standalone token rewrites of gscport.py (module functions before the
// Translator class), with the same names in CamelCase.

#include "util.h"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace w2b::gsc::port
{
    void NeutralizeAnimations(Tokens& tokens, const std::string& tree);
    int BridgeContactKillAttacks(Tokens& tokens, const Script& script);
    int FixSyntax(Tokens& tokens);
    int SplitCallReceivers(Tokens& tokens);
    const std::map<std::string, std::string>& WawLevelFields();
    int RenameLevelFields(Tokens& tokens);
    std::set<std::string> LevelFieldsRead(const Tokens& tokens, long long start = 2, std::optional<long long> end = std::nullopt);
    std::set<std::string> LevelFieldsAssigned(const Tokens& tokens, long long start = 2, std::optional<long long> end = std::nullopt);
    std::optional<std::string> GuardedLevelDefault(const Tokens& tokens);
    int BridgePoiRecordComparisons(Tokens& tokens);
    int BridgeReviveReads(Tokens& tokens);
    int BridgeDrinkingFlag(Tokens& tokens);
    int BridgeScoreWrites(Tokens& tokens);
    int BridgePlayerStatReads(Tokens& tokens);
    int BridgeDynamicHudMaterials(Tokens& tokens);
    int MirrorTeamZombieVars(Tokens& tokens);
    std::string HudMaterialTableSource(const std::map<std::string, std::string>& renamed);
    int BridgeFontScales(Tokens& tokens);
    bool BridgeHudCleanup(Tokens& tokens, long long index, const std::string& wrapper = "waw_destroy_hud_elem");
    int KeepBo2Deathanims(Tokens& tokens);
    bool UsesAnimations(const Tokens& tokens, long long start, long long end);
    void TruncateArgs(Tokens& tokens, long long nameIndex, int keep);
    std::optional<int> Argc(const Tokens& tokens, long long nameIndex);
    int BridgeIntermissionCamera(Tokens& tokens);
    int BridgeLaunchTriggers(Tokens& tokens);
    int LowerMaxhealthAssignments(Tokens& tokens);
    int LowerHealthAssignments(Tokens& tokens);
    int LowerFieldAssignments(Tokens& tokens, const std::string& field, const std::string& helper);
    int LowerHudFields(Tokens& tokens);
    int LowerZombieWeaponKeys(Tokens& tokens);
    std::optional<long long> CloseParen(const Tokens& tokens, long long start);
    std::optional<long long> SizeOwner(const Tokens& tokens, long long dot, long long floor);
    int LowerUndefinedSizeLoops(Tokens& tokens);
    int WrapIntegerLiterals(Tokens& tokens);
    int LowerStationaryFxCarriers(Tokens& tokens);

    bool SourceDrives(const std::string& core, const std::string& name);
    std::string PortedPath(const std::string& path);
    std::string AnimationTreeName(const std::string& tree);
    std::string AnimationAssetName(const std::string& name);
    std::set<std::string> AnimationNames(const Tokens& tokens);
    inline std::string ShadowedName(const std::string& name)
    {
        return "waw_" + name;
    }
} // namespace w2b::gsc::port

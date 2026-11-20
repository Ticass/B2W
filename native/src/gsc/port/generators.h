#pragma once
// Generated scripts and BO2 template hooks
// (assets_source ... hook_bo2_perk_client).

#include "sources.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace w2b::gsc::port
{
    inline constexpr const char* BO2_WEAPONS_SCRIPT = "_waw2bo2_bo2weapons.gsc";
    inline constexpr const char* LOADOUT = "maps\\_loadout";
    inline constexpr const char* CHARACTERS = "maps\\mp\\waw\\_waw2bo2_characters";
    inline constexpr const char* WEAPONS = "maps\\mp\\waw\\_waw2bo2_weapons";
    inline constexpr const char* PERK_OVERRIDE_HEADER = "// waw2bo2: WaW owns perk gameplay; BO2 runtime support only.\n";
    inline constexpr const char* PRE_ANCHOR = "    maps\\mp\\zombies\\_zm::init();";
    inline constexpr const char* BO2_FRAMEWORK_WEAPONS[] = {"knife_zm", "frag_grenade_zm", "m1911_zm", "m1911_upgraded_zm"};
    inline constexpr const char* BO2_PERK_OVERRIDES[] = {"maps/mp/zombies/_zm_perks.gsc", "clientscripts/mp/zombies/_zm_perks.csc"};

    // WEAPON_REGISTRATION: role -> (script, function), in its dict order
    const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>& WeaponRegistration();
    // role -> extracted function names (nullopt: extract_entry gave None)
    using Registration = std::vector<std::pair<std::string, std::vector<std::optional<std::string>>>>;

    std::string AssetsSource(const std::map<std::string, std::string>& fxTable, const std::optional<std::string>& wawMap = std::nullopt,
                             const std::map<std::string, std::string>& weapons = {}, const std::set<std::string>& boltedLoops = {},
                             const std::map<std::string, std::string>& zombieVarOverrides = {}, const std::map<std::string, std::string>& tables = {});
    std::string Bo2WeaponsSource(const std::set<std::string>& weapons = {});
    bool HasWawAppearance(Sources& sources);
    std::string CharactersSource();
    std::optional<std::pair<std::string, std::set<std::string>>> WawPackAPunchSource(const std::string& source, const std::string& path);
    std::string TeslaCallbackSource(const std::map<std::string, std::string>& entries);
    std::string WeaponsSource(const Registration& registration, const std::vector<std::string>& melee = {});
    bool HookBo2Box(const std::filesystem::path& mainGsc);
    bool HookBo2Weapons(const std::filesystem::path& mainGsc);
    bool HookBo2ClientCharacters(const std::filesystem::path& mainCsc);
    bool HookBo2Characters(const std::filesystem::path& mainGsc);
    std::string AnimtreesSource(const std::vector<std::string>& trees);
    bool HookBo2Animtrees(const std::filesystem::path& mainGsc, const std::filesystem::path& mainCsc, const std::filesystem::path& outRoot,
                          const std::vector<std::string>& trees);
    std::vector<std::string> DropPreservedTemplateInits(const std::filesystem::path& mainGsc, const std::filesystem::path& outRoot,
                                                        const std::optional<std::filesystem::path>& mainCsc = std::nullopt);
    bool HookBo2Main(const std::filesystem::path& mainGsc, const std::string& mapName);
    std::string PrecacheSource(const std::vector<std::string>& models);
    std::pair<std::string, std::vector<std::string>> GuardMissingModelCalls(const std::string& source, const std::set<std::string>& missing);
    std::vector<std::string> Bo2Scripts(const std::filesystem::path& outRoot);
    std::filesystem::path EnsureOwnedClientBootstrap(const std::filesystem::path& outRoot, const std::filesystem::path& bo2Root);
    std::string WawPvpAttackNotify(const std::string& bootstrap);
    std::string WawZombieVarDefaults(const std::string& bootstrap);
    std::string OwnedDogsSource(const std::filesystem::path& bo2Root);
    void StageBo2PerkSupport(const std::filesystem::path& outRoot, const std::filesystem::path& bo2Root, bool bo2StockPerks = false);
    void HookBo2PerkServer(const std::filesystem::path& mainGsc);
    void HookBo2PerkClient(const std::filesystem::path& mainCsc);

    // Path.read_text(encoding="utf-8", errors="replace")
    std::string ReadTextReplace(const std::filesystem::path& path);
} // namespace w2b::gsc::port

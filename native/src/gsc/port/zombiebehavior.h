#pragma once
// WaW speed selection and plain spawner locations
// on the BO2 AI runtime.

#include "sources.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace w2b::gsc::port::zombiebehavior
{
    inline constexpr const char* RUNTIME = "maps\\mp\\waw\\_waw2bo2_zombie_behavior";
    inline constexpr const char* SCRIPTED_STATE = "waw_scripted";
    inline constexpr const char* SCRIPTED_NOTIFY = "waw_scripted_anim";
    inline constexpr int SCRIPTED_CHUNK = 60;
    inline constexpr const char* SCRIPTED_TREE = "waw2bo2_generic_human";
    inline constexpr const char* ACTOR_TREE = "waw2bo2_zombie_actor";
    inline constexpr const char* SELECTION = "maps\\_waw2bo2_source_spawns";
    inline constexpr const char* SPEED_STATE = "maps\\_waw2bo2_source_speed_state";
    inline constexpr const char* CHASE_POLICY = "maps\\_waw2bo2_source_chase_policy";

    struct LastZombie
    {
        long long afterRound = 0;
        std::string speed;
    };

    // speed -> clips, in the order the source first assigns each speed
    using MovementSets = std::vector<std::pair<std::string, std::vector<std::string>>>;

    std::optional<std::string> SourceChasePolicy(Sources& sources, const util::Json& definition);
    std::optional<std::string> SourceSpeedState(Sources& sources, const util::Json& definition, bool rounds = true);
    std::optional<std::string> SourceSelection(Sources& sources, const util::Json& definition);
    // speed -> picked variants; nullopt = every authored entry reachable
    std::vector<std::pair<std::string, std::optional<std::set<long long>>>> ReachableVariants(Sources& sources, const util::Json& definition);
    std::optional<LastZombie> SourceLastZombie(Sources& sources, const util::Json& definition);
    std::optional<bool> FoldLevelGuard(const std::vector<std::string>& words, const std::string& mapName, const std::set<std::string>* assigned,
                                       const std::set<std::string>* unsetDvars = nullptr);
    std::pair<std::optional<std::string>, MovementSets> MovementSetsOf(Sources& sources, const util::Json& definition,
                                                                       const std::optional<std::string>& mapName = std::nullopt);
    MovementSets MeleeSets(Sources& sources, const util::Json& definition, const std::optional<std::string>& mapName = std::nullopt);

    using FindXanim = std::function<std::optional<std::filesystem::path>(const std::string&)>;
    std::tuple<std::string, std::vector<std::string>, std::vector<std::string>> SourceTraversalClips(std::string asd, const FindXanim& find,
                                                                                                      const std::filesystem::path& outRoot);
    util::Json StageActorMovement(Sources& sources, const std::filesystem::path& outRoot, const std::filesystem::path& bo2Root, const util::Json& definition,
                                const std::optional<std::string>& mapName = std::nullopt, const FindXanim* find = nullptr);

    std::string RuntimeSource(const util::Json& definition, const std::optional<std::string>& speedFunction,
                              const std::optional<std::string>& selectionFunction = std::nullopt,
                              const std::optional<std::string>& speedStateFunction = std::nullopt, bool sourceMembership = false,
                              const std::optional<std::string>& respawnFunction = std::nullopt,
                              const std::optional<std::string>& chaseFunction = std::nullopt, const std::optional<LastZombie>& lastZombie = std::nullopt,
                              std::vector<std::string> scriptedClips = {});

    std::string WawQuoteRules(std::string text, const std::string& native, const std::string& coreText);
    std::string WawDropRules(std::string text, const std::string& native);
    void StageNativeSupport(const std::filesystem::path& outRoot, const std::filesystem::path& bo2Root, const util::Json& definition);

    // f-string style template: {key} from values, {{ and }} literal braces
    std::string Format(std::string_view text, const std::map<std::string, std::string>& values);
} // namespace w2b::gsc::port::zombiebehavior

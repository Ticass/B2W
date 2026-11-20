#pragma once
// gscport.Translator: classifies and translates WaW scripts for BO2, extracts
// framework functions and generates the hook code.

#include "report.h"
#include "sources.h"
#include "statements.h"
#include "t6api.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::gsc::port
{
    struct ListenerCopy
    {
        std::string entry, effects, code;
    };

    class Translator
    {
    public:
        Translator(Sources& sources, const T6Api& api, PortReport& report);

        // ---- classification / resolution (translator_resolve.cpp)
        std::optional<std::string> Owner(const Script& script, const std::string& name);
        void Want(const std::string& path);
        std::optional<std::string> LibraryFunction(const std::string& script, const std::string& name);
        std::optional<std::string> CompatName(const std::string& name) const;
        std::optional<std::string> Bo2Unique(const std::string& name) const;
        static std::string StubName(const std::string& desc);
        std::string Stub(const std::string& desc, const std::string& name, std::optional<int> argc, const std::string& where);
        bool PlaysArgumentSound(const std::string& core, const std::string& name, const std::set<std::string>& seen = {});
        std::optional<std::string> FunctionScript(const std::string& core, const Ref& ref);
        std::optional<std::string> ResolveCore(const std::string& core, const std::string& name, std::optional<int> argc, const std::string& where,
                                               const std::set<std::string>& included, bool sibling = false);
        std::optional<std::string> ResolveBuiltin(const std::string& name, std::optional<int> argc, const std::string& where,
                                                  const std::set<std::string>& included, std::optional<bool> method = std::nullopt);
        bool ShadowsBuiltin(const std::string& name) const;
        bool SignatureDiffers(const std::string& core, const std::string& name);

        // ---- translation (translator_translate.cpp)
        std::string Translate(const std::string& path, bool mainSplit = false);
        void AdaptNativeArguments(Tokens& tokens, const Ref& ref, const std::string& native, std::optional<int> argc, const std::string& where);
        void ResolveRefs(Tokens& tokens, const Script& script, const std::string& whereFile, const std::set<std::string>& includedBo2,
                         const std::set<size_t>& skip, const std::optional<std::string>& extracting = std::nullopt,
                         const std::vector<std::pair<long long, long long>>* neutralizedRanges = nullptr);
        void RewriteAnimationRefs(Tokens& tokens, const std::string& tree, const std::string& where, long long start = 0,
                                  std::optional<long long> end = std::nullopt);
        void KeepCoreAnimations(const Script& script, Tokens& tokens, const std::string& where, const std::optional<std::string>& animtree);
        std::string GuardSizeLoops(const std::string& text);
        void SourceLowerings(Tokens& tokens);
        std::string TranslateExtracted(const std::string& core, const std::string& name);

        // ---- extraction (translator_extract.cpp)
        std::optional<std::string> Extract(const std::string& core, const std::string& name);
        std::optional<std::string> ExtractEntry(const std::string& core, const std::string& name);
        std::optional<std::string> ExtractLevelState(const std::string& core, const std::string& init,
                                                     const std::optional<std::set<std::string>>& read = std::nullopt,
                                                     const std::optional<std::set<std::string>>& effectKeys = std::nullopt);
        std::optional<std::string> ExtractSpawnerLists();
        std::optional<std::string> ExtractEndGameMusic();
        std::optional<std::string> ExtractPlayerConnectFields();
        std::optional<std::string> ExtractDamagePrelude(const std::string& core = "", const std::string& function = "");
        std::optional<std::string> ExtractMainLevelState();
        std::optional<std::string> ExtractSourcePlayerDamage();
        std::optional<std::string> ExtractRoundHealth(const std::string& core = "", const std::string& function = "", bool sourceLoop = false);
        std::optional<std::string> ExtractRoundCounter(const std::string& core = "", bool sourceLoop = false);
        std::optional<std::vector<std::string>> RoundLoopMissingAnchors(const std::string& core = "", const std::string& function = "round_think");
        std::optional<std::string> ExtractRoundLoop(const std::string& core = "", const std::string& function = "round_think");
        bool RoundStartDrives(const std::string& core = "");
        std::string ExtractRoundStartDriver(const std::string& core, const std::string& where, const std::vector<std::string>& missing);
        const Function* MapFrameworkFunction(const std::string& name);
        std::optional<std::string> ExtractCustomIntermission();
        std::vector<std::string> ExtractEndGame();

        // ---- hooks (translator_hooks.cpp)
        std::vector<std::string> JuggernogHooks();
        std::vector<std::pair<std::string, std::vector<std::string>>> AnnouncerLines(const std::string& core, const std::string& function, int depth = 5);
        std::vector<std::string> PowerupModelsHook();
        std::vector<std::string> NativeReplacements();
        std::vector<std::string> QuoteHooks();
        std::vector<std::string> ZombieVocalsHook();
        std::vector<std::string> SoundTableHook();
        std::vector<std::string> AnnouncerHook(const std::filesystem::path& outRoot);
        std::vector<std::string> SkillSignalsHook();
        std::vector<std::string> ExplodersHook();
        std::optional<std::string> DogRoundsHook();
        std::vector<std::string> ClientFogMonitors(const util::Json& moments);
        std::optional<std::string> FrameworkZombieVars();
        std::vector<std::string> ClientFxListeners();
        std::optional<ListenerCopy> ClientFxListener(const std::string& key, const Script& script, const std::string& entry,
                                                     const std::map<std::string, std::string>& effects);
        std::vector<std::string> FrameworkHooks();
        void SplitMain(Script& script);
        std::string StubSource() const;

        // ---- state
        Sources& sources;
        const T6Api& api;
        PortReport& report;
        Script compat;
        std::vector<std::string> queue;
        std::vector<std::pair<std::string, std::string>> done; // waw path -> BO2 source text
        std::map<std::string, std::pair<std::string, int>> stubs;
        std::map<std::pair<std::string, std::string>, std::string> coreFuncs;
        std::vector<std::pair<std::string, std::string>> coreQueue;
        std::set<std::string> animtrees;
        std::map<std::string, std::set<std::string>> coreAnimtrees;
        std::vector<std::string> levelStateParts;
        bool animNeutralized = false;
        bool animationDiscovery = false;
        std::map<std::string, std::set<std::string>> animationRequirements;
        bool bo2StockPerks = false;
        bool stockMap = false;
        std::map<std::string, std::set<std::string>> wawPerkParts;
        std::vector<std::string> postCalls, preCalls;
        std::set<std::pair<std::string, std::string>> intermissionFuncs;
        std::map<std::pair<std::string, std::string>, std::function<std::string(const std::string&)>> postPatches;
        std::optional<std::string> mapName;
        std::set<std::string> levelStateNames;
        std::map<std::pair<std::string, std::string>, std::optional<ListenerCopy>> listenerCopies;
        std::set<std::pair<std::string, std::string>> listenerFunctions;

        bool IsDone(const std::string& path) const;

    private:
        std::optional<ListenerCopy> ClientFxListenerCopy(const std::string& key, const Script& script, const std::string& entry,
                                                         const std::map<std::string, std::string>& effects);
        static void DropArgument(Tokens& toks, long long nameIndex, size_t index);
    };

    // API.get(key, {}) without a copy: the section, or a null value when absent
    const util::Json& ApiSection(const std::string& key);
    // a lookup on gsc_api.json: nullptr when absent or null
    const util::Json* ApiGet(const util::Json& object, const std::string& key);
    std::optional<std::string> ApiString(const util::Json& object, const std::string& key);
    bool ApiListHas(const util::Json& list, const std::string& value);

    inline constexpr const char* CORE_INCLUDES[] = {"common_scripts\\utility", "maps\\mp\\_utility", "maps\\mp\\zombies\\_zm_utility"};
    inline constexpr const char* CALLBACKGLOBAL = "maps\\_callbackglobal";
    inline constexpr const char* LASTSTAND = "maps\\_laststand";
    inline constexpr const char* DAMAGE_OVERRIDE = "player_damage_override";
    inline constexpr const char* ROUND_SPAWNING = "round_spawning";
    inline constexpr int END_GAME_FALLBACK = 30;
    std::set<std::string> CoreIncludes();
} // namespace w2b::gsc::port

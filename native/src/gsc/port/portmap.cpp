// Port of gscport.port_map and its linker passes (see portmap.h).
#include "portmap.h"

#include "api.h"
#include "core/files.h"
#include "core/regex.h"
#include "generators.h"
#include "rewrites.h"
#include "support.h"
#include "translator.h"
#include "zombiebehavior.h"

#include <algorithm>
#include <stdexcept>

namespace w2b::gsc::port
{
    namespace fs = std::filesystem;

    namespace
    {
        const std::pair<const char*, const char*> BOX_STATE_INIT{"maps\\_zombiemode_weapons", "init"};
        const std::pair<const char*, const char*> FRAMEWORK_STATE_INITS[] = {{"maps\\_zombiemode", "init_standard_zombie_anims"}, {"maps\\_zombiemode", "init_anims"}};
        const std::pair<const char*, const char*> FRAMEWORK_EFFECT_INITS[] = {{"maps\\_zombiemode", "init_fx"}};
        const std::pair<const char*, const char*> DAMAGE_PRELUDES[] = {{"maps\\_zombiemode", "player_damage_override"}, {"maps\\_callbackglobal", "callback_playerdamage"}};
        const std::pair<const char*, const char*> ALTMELEE_WEAPONS[] = {{"specialty_altmelee", "bowie_knife_zm"}};

        bool Ok(const std::optional<std::string>& value)
        {
            return value && !value->empty();
        }

        std::string S(const util::Json& value)
        {
            return value.get<std::string>();
        }

        std::string Posix(std::string path)
        {
            return Replace(std::move(path), "\\", "/");
        }

        bool Done(const Translator& tr, const std::string& path)
        {
            for (const auto& [p, _] : tr.done)
                if (p == path)
                    return true;
            return false;
        }

        std::set<std::string> ReadFields(const Translator& tr, const std::vector<std::string>& parts)
        {
            std::set<std::string> read;
            for (const auto& [_, text] : tr.done)
            {
                const auto fields = LevelFieldsRead(Tokenize(text));
                read.insert(fields.begin(), fields.end());
            }
            for (const auto& text : parts)
            {
                const auto fields = LevelFieldsRead(Tokenize(text));
                read.insert(fields.begin(), fields.end());
            }
            return read;
        }

        // sources.text = dict(sources.text) (+ parsed / origin copies)
        void OwnText(Sources& sources)
        {
            sources.text = std::make_shared<TextDict>(*sources.text);
        }
        void OwnParsed(Sources& sources)
        {
            sources.parsed = std::make_shared<std::unordered_map<std::string, ScriptPtr>>(*sources.parsed);
        }
        void OwnOrigin(Sources& sources)
        {
            sources.origin = std::make_shared<TextDict>(*sources.origin);
        }

        void Drop(Sources& sources, const std::string& key)
        {
            sources.parsed->erase(key);
        }
    } // namespace

    Sources DeepCopy(const Sources& sources)
    {
        Sources copy = sources;
        copy.text = std::make_shared<TextDict>(*sources.text);
        copy.origin = std::make_shared<TextDict>(*sources.origin);
        copy.stock = std::make_shared<TextDict>(*sources.stock);
        copy.parsed = std::make_shared<std::unordered_map<std::string, ScriptPtr>>();
        for (const auto& [key, script] : *sources.parsed)
            (*copy.parsed)[key] = script ? std::make_shared<Script>(*script) : nullptr;
        return copy;
    }

    std::map<std::string, std::set<std::string>> DiscoverAnimationDependencies(const Sources& original, const T6Api& api, const std::string& mapName,
                                                                               const std::set<std::string>& actorTypes)
    {
        auto sources = DeepCopy(original);
        sources.BindZombieFramework("maps/" + mapName);
        for (const auto& name : actorTypes)
        {
            const auto path = "aitype\\" + name;
            const auto* text = sources.text->Find(path + ".gsc");
            if (!text)
                text = sources.stock->Find(path + ".gsc");
            if (text)
            {
                sources.text->Set(path + ".gsc", zombieappearance::AppearanceScript(*text));
                Drop(sources, path + ".gsc");
            }
        }
        PortReport report;
        Translator tr(sources, api, report);
        tr.mapName = mapName;
        tr.animationDiscovery = true;
        const auto main = Norm("maps/" + mapName);
        tr.queue.push_back(main);
        tr.ExtractCustomIntermission();
        tr.ExtractEndGame();
        tr.ExtractEntry(ZOMBIEMODE, "init_strings");
        const auto& speed = Api()["zombie_runtime"]["source_speed"];
        tr.ExtractEntry(S(speed["script"]), S(speed["function"]));
        for (const auto& name : actorTypes)
            tr.Want("aitype\\" + name);
        if (HasWawAppearance(sources))
            tr.Want(LOADOUT);
        for (const auto& [_, entries] : WeaponRegistration())
            for (const auto& [core, name] : entries)
                tr.ExtractEntry(core, name);
        std::vector<std::string> parts;
        const auto drain = [&]
        {
            while (!tr.queue.empty() || !tr.coreQueue.empty())
            {
                if (!tr.coreQueue.empty())
                {
                    const auto [core, name] = tr.coreQueue.front();
                    tr.coreQueue.erase(tr.coreQueue.begin());
                    parts.push_back(tr.TranslateExtracted(core, name));
                }
                else
                {
                    const auto path = tr.queue.front();
                    tr.queue.erase(tr.queue.begin());
                    if (!Done(tr, path))
                    {
                        auto text = tr.Translate(path, path == main);
                        tr.done.emplace_back(path, std::move(text));
                    }
                }
            }
        };
        drain();
        const auto read = ReadFields(tr, parts);
        tr.ExtractLevelState(BOX_STATE_INIT.first, BOX_STATE_INIT.second, read);
        for (const auto& [core, name] : FRAMEWORK_STATE_INITS)
            tr.ExtractLevelState(core, name, read);
        drain();
        std::map<std::string, std::set<std::string>> result;
        for (const auto& [tree, names] : tr.animationRequirements)
            if (!names.empty())
                result[tree] = names;
        return result;
    }

    namespace
    {
        thread_local std::string g_phase;
        void Phase(std::string text)
        {
            g_phase = std::move(text);
        }
    } // namespace

    static PortReport PortMapImpl(const Sources& input, const T6Api& api, const std::string& mapName, const fs::path& outRoot, const PortMapOptions& options)
    {
        Phase("setup");
        Sources sources = input;
        const auto frameworkAliases = sources.BindZombieFramework("maps/" + mapName);
        std::map<std::string, std::string> actorKinds;
        if (!options.actorTypes.empty())
        {
            OwnText(sources);
            sources.parsed = std::make_shared<std::unordered_map<std::string, ScriptPtr>>();
            for (const auto& name : options.actorTypes)
            {
                const auto path = "aitype\\" + name;
                if (!sources.Get(path))
                    throw std::runtime_error("WaW zombie actor " + path + ".gsc missing: donor appearance is forbidden");
                const auto* text = sources.text->Find(path + ".gsc");
                if (!text)
                    text = sources.stock->Find(path + ".gsc");
                const auto original = *text;
                actorKinds[name] = zombieappearance::SourceActorKind(original);
                sources.text->Set(path + ".gsc", zombieappearance::AppearanceScript(original));
                Drop(sources, path + ".gsc");
            }
        }
        if (options.bo2StockPerks)
        {
            auto text = std::make_shared<TextDict>();
            for (const auto& key : sources.text->order)
                text->Set(key, stockperks::TranslateLiterals(sources.text->values.at(key)));
            auto stock = std::make_shared<TextDict>();
            for (const auto& key : sources.stock->order)
                stock->Set(key, stockperks::TranslateLiterals(sources.stock->values.at(key)));
            sources.text = text;
            sources.stock = stock;
            sources.parsed = std::make_shared<std::unordered_map<std::string, ScriptPtr>>();
        }
        std::map<std::string, std::set<std::string>> wawPerkParts;
        if (options.bo2StockPerks)
        {
            std::set<std::string> keys(sources.text->order.begin(), sources.text->order.end());
            keys.insert(sources.stock->order.begin(), sources.stock->order.end());
            for (const auto& key : keys)
            {
                if (!EndsWith(key, ".gsc") || !stockperks::IsPerkFramework(Norm(key)))
                    continue;
                const auto* text = sources.text->Find(key);
                if (!text)
                    text = sources.stock->Find(key);
                if (auto kept = WawPackAPunchSource(*text, Norm(key)))
                {
                    sources.text->Set(key, kept->first);
                    wawPerkParts[Norm(key)] = kept->second;
                }
            }
        }
        PortReport report;
        report.mapMain = "maps\\" + mapName;
        for (const auto& [alias, canonical] : frameworkAliases)
            report.rewrites["framework startup " + alias + " -> " + canonical] += 1;
        Translator tr(sources, api, report);
        tr.mapName = mapName;
        tr.bo2StockPerks = options.bo2StockPerks;
        tr.stockMap = options.stockMap;
        tr.wawPerkParts = wawPerkParts;
        for (const auto& a : options.animtrees)
            tr.animtrees.insert(Lower(a));
        for (const auto& [t, names] : options.coreAnimtrees)
            for (const auto& a : names)
                tr.coreAnimtrees[Lower(t)].insert(Lower(a));
        const auto main = "maps\\" + mapName;
        if (!sources.Get(main))
        {
            report.errors.push_back("map main " + main + ".gsc not found in the map scripts");
            return report;
        }
        {
            std::vector<std::string> keys = sources.text->order;
            std::sort(keys.begin(), keys.end());
            for (const auto& key : keys)
            {
                const auto p = Norm(key);
                const auto* stock = sources.stock->Find(key);
                if (EndsWith(key, ".gsc") && IsCore(p, sources) && stock && sources.text->values.at(key) != *stock)
                    report.coreOverrides.push_back(p);
            }
        }
        const auto outDir = outRoot / "maps" / "mp" / "waw";
        std::error_code error;
        for (const auto& old : files::RGlob(outDir, ".gsc"))
            fs::remove(old, error);
        if (fs::exists(outRoot / "aitype", error))
            for (const auto& entry : fs::recursive_directory_iterator(outRoot / "aitype"))
            {
                const auto name = files::Utf8(entry.path().filename());
                if (!entry.is_regular_file() || !StartsWith(Lower(name), "waw_"))
                    continue;
                if (StartsWith(files::ReadText(entry.path()), "// waw2bo2:"))
                    fs::remove(entry.path(), error);
            }
        tr.queue.push_back(main);
        Phase("tr.ExtractCustomIntermission();");
        tr.ExtractCustomIntermission();
        tr.ExtractEndGame();
        const auto append = [](std::vector<std::string>& to, const std::vector<std::string>& from) { to.insert(to.end(), from.begin(), from.end()); };
        append(tr.postCalls, tr.ExplodersHook());
        append(tr.postCalls, tr.JuggernogHooks());
        append(tr.postCalls, tr.AnnouncerHook(outRoot));
        append(tr.postCalls, tr.PowerupModelsHook());
        append(tr.postCalls, tr.SoundTableHook());
        append(tr.postCalls, tr.ZombieVocalsHook());
        append(tr.postCalls, tr.QuoteHooks());
        append(tr.preCalls, tr.NativeReplacements());
        append(tr.preCalls, tr.SkillSignalsHook());
        if (const auto state = tr.ExtractLevelState(ZOMBIEMODE, "main", std::set<std::string>{"global_damage_func", "global_damage_func_ads"}); Ok(state))
            tr.postCalls.push_back(std::string(CORE) + "::" + *state + "();");
        for (const auto& name : options.actorTypes)
            tr.Want("aitype\\" + name);
        Phase("const auto hintRegistry");
        const auto hintRegistry = tr.ExtractEntry("maps\\_zombiemode", "init_strings");
        const auto& runtime = Api()["zombie_runtime"];
        const auto& speedDefinition = runtime["source_speed"];
        const auto speedScript = sources.Get(S(speedDefinition["script"]));
        const Function* speedEntry = speedScript ? speedScript->Find(S(speedDefinition["function"])) : nullptr;
        std::optional<std::string> speedFunction;
        if (speedEntry)
        {
            if (static_cast<long long>(speedEntry->params.size()) != speedDefinition["arguments"].get<long long>())
                report.errors.push_back(S(speedDefinition["script"]) + "::" + S(speedDefinition["function"]) + ": source speed selector must accept zero arguments");
            else
            {
                speedFunction = tr.ExtractEntry(S(speedDefinition["script"]), S(speedDefinition["function"]));
                report.rewrites["source WaW zombie speed selector -> native spawn callback"] += 1;
            }
        }
        tr.postCalls.insert(tr.postCalls.begin(), std::string(zombiebehavior::RUNTIME) + "::init();");
        std::optional<std::string> speedStateFunction;
        Phase("const auto missingAnchors");
        const auto missingAnchors = tr.RoundLoopMissingAnchors();
        const bool sourceLoop = (missingAnchors && missingAnchors->empty() && SourceDrives(ZOMBIEMODE, "round_spawning")) || tr.RoundStartDrives();
        if (const auto speedState = zombiebehavior::SourceSpeedState(sources, runtime, !sourceLoop); Ok(speedState))
        {
            OwnText(sources);
            OwnParsed(sources);
            const auto key = std::string(zombiebehavior::SPEED_STATE) + ".gsc";
            sources.text->Set(key, *speedState);
            sources.origin->Set(key, "source speed initialization and round inputs");
            Drop(sources, key);
            tr.Want(zombiebehavior::SPEED_STATE);
            speedStateFunction = PortedPath(zombiebehavior::SPEED_STATE);
            report.rewrites["source WaW speed round inputs -> shared runtime"] += 1;
        }
        const auto lastZombie = zombiebehavior::SourceLastZombie(sources, runtime);
        report.rewrites[lastZombie ? "source last-zombie speed rule -> native zombie_speed_up" : "no source last-zombie speed rule: native zombie_speed_up disabled"] += 1;
        const auto selectionSource = zombiebehavior::SourceSelection(sources, runtime);
        const auto chaseSource = zombiebehavior::SourceChasePolicy(sources, runtime);
        std::optional<std::string> chaseFunction;
        if (Ok(chaseSource))
        {
            OwnText(sources);
            OwnParsed(sources);
            OwnOrigin(sources);
            const auto key = std::string(zombiebehavior::CHASE_POLICY) + ".gsc";
            sources.text->Set(key, *chaseSource);
            sources.origin->Set(key, "source zombie chase/entrance policy");
            Drop(sources, key);
            tr.Want(zombiebehavior::CHASE_POLICY);
            chaseFunction = PortedPath(zombiebehavior::CHASE_POLICY) + "::should_chase";
            report.rewrites["source zombie chase/entrance policy -> native AI"] += 1;
        }
        std::optional<std::string> selectionFunction;
        if (Ok(selectionSource))
        {
            OwnText(sources);
            OwnParsed(sources);
            OwnOrigin(sources);
            const auto key = std::string(zombiebehavior::SELECTION) + ".gsc";
            sources.text->Set(key, *selectionSource);
            const auto& contract = runtime["source_selection"];
            const auto* origin = sources.origin->Find(S(contract["script"]) + ".gsc");
            sources.origin->Set(key, (origin ? *origin : S(contract["script"])) + "::" + S(contract["function"]) + " (selection statements)");
            Drop(sources, key);
            tr.Want(zombiebehavior::SELECTION);
            selectionFunction = PortedPath(zombiebehavior::SELECTION) + "::choose";
            report.rewrites["source WaW spawner selection -> native round spawning"] += 1;
        }
        Phase("Registration registration;");
        Registration registration;
        for (const auto& [role, entries] : WeaponRegistration())
        {
            std::vector<std::optional<std::string>> functions;
            for (const auto& [core, fn] : entries)
                if (const auto script = sources.Get(core); script && script->Find(fn))
                    functions.push_back(tr.ExtractEntry(core, fn));
            registration.emplace_back(role, functions);
        }
        const auto& teslaDefinition = Api()["tesla_runtime"];
        const auto tesla = S(teslaDefinition["source_script"]);
        const auto teslaScript = sources.Get(tesla);
        std::map<std::string, std::string> teslaEntries;
        const char* teslaKeys[] = {"initialize", "damage_predicate", "damage_entry"};
        bool teslaComplete = teslaScript != nullptr;
        for (const char* key : teslaKeys)
            teslaComplete = teslaComplete && teslaScript->Find(S(teslaDefinition[key]));
        if (teslaComplete)
        {
            for (const char* key : teslaKeys)
                teslaEntries[key] = tr.ExtractEntry(tesla, S(teslaDefinition[key])).value_or("None");
            tr.postCalls.push_back(std::string(CORE) + "::waw_tesla_init();");
            report.rewrites["source Tesla chain -> native zombie damage callback"] += 1;
        }
        const bool appearance = HasWawAppearance(sources);
        if (appearance)
            tr.Want(LOADOUT);
        else
            report.errors.push_back(std::string(LOADOUT) + ".gsc with init_loadout/give_model not found (map or stock): players keep the BO2 template's characters");
        std::vector<std::string> coreParts;
        static const regex::Regex loadfx(R"re(loadfx\s*\(\s*"([^"]+)")re", regex::IGNORECASE);
        const auto drain = [&]
        {
            while (!tr.queue.empty() || !tr.coreQueue.empty())
            {
                if (!tr.coreQueue.empty())
                {
                    const auto [core, name] = tr.coreQueue.front();
                    tr.coreQueue.erase(tr.coreQueue.begin());
                    Phase("translate extracted " + core + "::" + name);
                    coreParts.push_back(tr.TranslateExtracted(core, name));
                    continue;
                }
                const auto path = tr.queue.front();
                tr.queue.erase(tr.queue.begin());
                if (Done(tr, path))
                    continue;
                Phase("translate " + path);
                auto text = tr.Translate(path, path == main);
                tr.done.emplace_back(path, text);
                files::WriteText(outRoot / files::FromUtf8(Posix(PortedPath(path)) + ".gsc"), text);
                report.ported.push_back(path);
                std::set<std::string> effects;
                if (const auto* source = sources.text->Find(path + ".gsc"))
                    for (const auto& row : loadfx.FindAll(*source))
                        effects.insert(row[0]);
                report.fx.insert(report.fx.end(), effects.begin(), effects.end());
            }
        };
        drain();
        if (!teslaEntries.empty())
            coreParts.push_back(TeslaCallbackSource(teslaEntries));
        const auto hasRole = [&](const std::string& role)
        {
            for (const auto& [r, fns] : registration)
                if (r == role && !fns.empty())
                    return true;
            return false;
        };
        if (hasRole("box"))
            if (const auto state = tr.ExtractLevelState(BOX_STATE_INIT.first, BOX_STATE_INIT.second); Ok(state))
            {
                registration.emplace_back("box_state", std::vector<std::optional<std::string>>{state});
                drain();
            }
        Phase("const auto read = ReadFields(tr, coreParts);");
        const auto read = ReadFields(tr, coreParts);
        std::vector<std::string> frameworkState;
        for (const auto& [core, init] : FRAMEWORK_STATE_INITS)
            if (const auto state = tr.ExtractLevelState(core, init, read); Ok(state))
                frameworkState.push_back(*state);
        const auto& selectionContract = runtime["source_selection"];
        const auto membershipField = S(selectionContract["array_field"]);
        bool sourceMembership = false;
        if (Ok(selectionFunction) && read.count(membershipField))
            if (const auto state = tr.ExtractLevelState(S(selectionContract["script"]), S(runtime["source_membership"]["initializer"]), std::set<std::string>{membershipField});
                Ok(state))
            {
                frameworkState.push_back(*state);
                sourceMembership = true;
                report.rewrites["source active spawner membership -> shared runtime"] += 1;
            }
        const auto& respawnContract = runtime["source_respawn"];
        const auto respawnScript = sources.Get(S(respawnContract["script"]));
        std::optional<std::string> respawnFunction;
        if (respawnScript && respawnScript->Find(S(respawnContract["function"])))
            respawnFunction = tr.ExtractEntry(S(respawnContract["script"]), S(respawnContract["function"]));
        if (Ok(hintRegistry))
            frameworkState.push_back(*hintRegistry);
        if (const auto mainState = tr.ExtractMainLevelState(); Ok(mainState))
            frameworkState.insert(frameworkState.begin(), *mainState);
        if (const auto sourceDamage = tr.ExtractSourcePlayerDamage(); Ok(sourceDamage))
            frameworkState.push_back(*sourceDamage);
        else
            for (const auto& [core, function] : DAMAGE_PRELUDES)
                if (const auto prelude = tr.ExtractDamagePrelude(core, function); Ok(prelude))
                    frameworkState.push_back(*prelude);
        Phase("const auto roundLoop = tr.ExtractRoundLoop();");
        const auto roundLoop = tr.ExtractRoundLoop();
        if (Ok(roundLoop))
            frameworkState.push_back(*roundLoop);
        if (const auto health = tr.ExtractRoundHealth("", "", Ok(roundLoop)); Ok(health))
            frameworkState.push_back(*health);
        if (Ok(roundLoop) || options.stockMap)
            if (const auto counter = tr.ExtractRoundCounter("", Ok(roundLoop)); Ok(counter))
                frameworkState.push_back(*counter);
        std::vector<std::string> texts;
        for (const auto& [_, text] : tr.done)
            texts.push_back(text);
        texts.insert(texts.end(), coreParts.begin(), coreParts.end());
        static const regex::Regex effectRead(R"re(level\.waw_effect\[\s*"([^"]+)"\s*\])re");
        static const regex::Regex effectSet(R"re(level\.waw_effect\[\s*"([^"]+)"\s*\]\s*=[^=])re");
        std::set<std::string> effectReads, effectSets;
        for (const auto& t : texts)
        {
            for (const auto& row : effectRead.FindAll(t))
                effectReads.insert(Lower(row[0]));
            for (const auto& row : effectSet.FindAll(t))
                effectSets.insert(Lower(row[0]));
        }
        Phase("if (!options.clientPorted)");
        if (!options.clientPorted)
            append(frameworkState, tr.ClientFxListeners());
        if (const auto zombieVars = tr.FrameworkZombieVars(); Ok(zombieVars))
            frameworkState.push_back(*zombieVars);
        std::set<std::string> missingEffects;
        std::set_difference(effectReads.begin(), effectReads.end(), effectSets.begin(), effectSets.end(), std::inserter(missingEffects, missingEffects.begin()));
        if (!missingEffects.empty())
            for (const auto& [core, init] : FRAMEWORK_EFFECT_INITS)
                if (const auto state = tr.ExtractLevelState(core, init, std::set<std::string>{"_effect"}, missingEffects); Ok(state))
                {
                    frameworkState.push_back(*state);
                    report.rewrites["framework effects registered for ported reads"] += 1;
                }
        drain();
        append(coreParts, tr.levelStateParts);
        static const regex::Regex wawLoadfx(R"re(waw_loadfx\s*\(\s*"([^"]+)")re", regex::IGNORECASE);
        for (const auto& row : wawLoadfx.FindAll(Join(coreParts, "\n")))
            report.fx.push_back(row[0]);
        std::string calls;
        for (const auto& fn : frameworkState)
            calls += "\t" + fn + "();\n";
        coreParts.push_back("\n// level state of WaW framework inits BO2 does not run (called by waw_main_pre)\nframework_level_state()\n{\n" + calls + "}\n");
        for (const char* script : {"_waw2bo2_compat.gsc", "_waw2bo2_fixes.gsc"})
            files::WriteText(outDir / script, files::ReadText(CompatDir() / script));
        files::WriteText(outDir / "_waw2bo2_hint_defaults.gsc", hintdefaults::Source(outRoot));
        Phase("std::vector<std::string> coreHead");
        std::vector<std::string> coreHead{"// waw2bo2: WaW framework functions BO2 has no counterpart for, extracted from the map's",
                                          "// framework scripts (its overrides, else stock WaW) and translated."};
        for (const char* inc : CORE_INCLUDES)
            coreHead.push_back(std::string("#include ") + inc + ";");
        coreHead.push_back("");
        files::WriteText(outDir / "_waw2bo2_core.gsc", Join(coreHead, "\n") + Join(coreParts, "\n") + "\n");
        files::WriteText(outDir / "_waw2bo2_stubs.gsc", tr.StubSource());
        files::WriteText(outDir / "_waw2bo2_hud_materials.gsc", HudMaterialTableSource({}));
        files::WriteText(outDir / "_waw2bo2_models.gsc", "table()\n{\n    return [];\n}\n");
        files::WriteText(outDir / "_waw2bo2_assets.gsc", AssetsSource(options.fxTable, mapName));
        files::WriteText(outDir / BO2_WEAPONS_SCRIPT, Bo2WeaponsSource());
        files::WriteText(outDir / "_waw2bo2_visions.gsc", "init()\n{\n    level.waw2bo2_visions = [];\n}\n");
        files::WriteText(outDir / perktable::SCRIPT, perktable::Source({}));
        files::WriteText(outDir / "_waw2bo2_precache.gsc", PrecacheSource({}));
        files::WriteText(outDir / "_waw2bo2_oneway.gsc", oneway::OnewaySource());
        files::WriteText(outDir / "_waw2bo2_water_bullets.gsc", waterbullets::WaterBulletsSource({}));
        if (appearance)
            files::WriteText(outDir / "_waw2bo2_characters.gsc", CharactersSource());
        std::vector<std::string> melee;
        for (const auto& [perk, weapon] : ALTMELEE_WEAPONS)
        {
            bool used = false;
            for (const auto& key : sources.text->order)
                used = used || Contains(sources.text->values.at(key), perk);
            if (used)
                melee.push_back(weapon);
        }
        for (const auto& weapon : melee)
            tr.report.rewrites["WaW altmelee perk -> BO2 " + weapon + " (included, not in the box)"] += 1;
        files::WriteText(outDir / "_waw2bo2_weapons.gsc", WeaponsSource(registration, melee));
        std::vector<std::string> scripted;
        if (const auto generic = options.coreAnimtrees.find("generic_human"); generic != options.coreAnimtrees.end())
            for (const auto& n : generic->second)
                scripted.push_back(AnimationAssetName(n));
        std::sort(scripted.begin(), scripted.end());
        files::WriteText(outDir / "_waw2bo2_zombie_behavior.gsc",
                         zombiebehavior::RuntimeSource(runtime, Ok(speedFunction) ? std::optional<std::string>(std::string(CORE) + "::" + *speedFunction) : std::nullopt,
                                                       Ok(selectionFunction) ? selectionFunction : std::nullopt, Ok(speedStateFunction) ? speedStateFunction : std::nullopt,
                                                       sourceMembership,
                                                       Ok(respawnFunction) ? std::optional<std::string>(std::string(CORE) + "::" + *respawnFunction) : std::nullopt,
                                                       Ok(chaseFunction) ? chaseFunction : std::nullopt, lastZombie, scripted));
        report.weaponRegistration = util::Json::object();
        for (const auto& [role, fns] : registration)
        {
            util::Json list = util::Json::array();
            for (const auto& f : fns)
                if (Ok(f))
                    list.push_back(*f);
            report.weaponRegistration[role] = list;
        }
        report.characters = appearance;
        {
            std::set<std::string> fx(report.fx.begin(), report.fx.end());
            report.fx.assign(fx.begin(), fx.end());
        }
        if (!options.actorTypes.empty())
        {
            const ActorTemplates templates = options.actorTemplate.value_or(ActorTemplates{});
            for (const auto& name : options.actorTypes)
            {
                const auto kindIt = actorKinds.find(name);
                const auto kind = kindIt == actorKinds.end() ? std::string("human") : kindIt->second;
                auto t = templates.find(kind);
                if (t == templates.end() || t->second.empty())
                    t = templates.find("human");
                if (t == templates.end())
                    throw std::runtime_error("T6 actor template missing");
                const auto& tmpl = t->second;
                if (!templates.count(kind))
                    report.errors.push_back("aitype\\" + name + ": no native T6 actor family for WaW AI type '" + kind + "'; built on the human zombie actor");
                const auto actor = zombieappearance::ActorName(name);
                files::WriteText(outRoot / "aitype" / files::FromUtf8(actor + ".gsc"), zombieappearance::NativeActor(tmpl, PortedPath("aitype\\" + name)));
                const auto tree = zombieappearance::TemplateTree(tmpl);
                files::WriteText(outRoot / "aitype/clientscripts" / files::FromUtf8(actor + ".csc"), zombieappearance::NativeClientActor(name, tree));
                if (templates.count(kind) && kind != "human")
                    report.nativeActorTrees[tree] = kind;
            }
        }
        Phase("FixArity(outRoot, api, &report);");
        FixArity(outRoot, api, &report);
        return report;
    }

    namespace
    {
        class Linker
        {
        public:
            Linker(const fs::path& outRoot, const T6Api& api) : api(api)
            {
                const auto base = outRoot / "maps" / "mp" / "waw";
                for (const auto& p : files::RGlob(base, ".gsc"))
                {
                    auto relative = files::Utf8(p.lexically_relative(base).replace_extension().generic_wstring());
                    const auto key = Lower("maps\\mp\\waw\\" + Replace(relative, "/", "\\"));
                    if (index.count(key))
                        continue;
                    index[key] = order.size();
                    order.push_back(key);
                    paths.push_back(p);
                    generated.push_back(Parse(files::ReadText(p), key));
                }
            }

            const Script* Generated(const std::string& key) const
            {
                const auto it = index.find(key);
                return it == index.end() ? nullptr : &generated[it->second];
            }

            std::optional<int> Params(const std::string& script, const std::string& name) const
            {
                const auto s = Lower(script);
                if (const Script* g = Generated(s))
                {
                    const Function* fn = g->Find(name);
                    return fn ? std::optional<int>(static_cast<int>(fn->params.size())) : std::nullopt;
                }
                const auto it = api.scriptIndex.find(s);
                if (it == api.scriptIndex.end())
                    return std::nullopt;
                const auto& functions = api.scripts[it->second].second;
                const auto f = functions.find(name);
                return f == functions.end() ? std::nullopt : std::optional<int>(f->second);
            }

            // ("script", params) / ("builtin", None) / ("missing", None)
            std::pair<std::string, std::optional<int>> Target(const Script& script, const Ref& ref) const
            {
                const auto name = Lower(ref.name);
                if (ref.qualifier)
                {
                    const auto n = Params(*ref.qualifier, name);
                    return n ? std::make_pair(std::string("script"), n) : std::make_pair(std::string("missing"), std::optional<int>());
                }
                if (const Function* fn = script.Find(name))
                    return {"script", static_cast<int>(fn->params.size())};
                for (const auto& inc : script.includes)
                    if (const auto n = Params(inc, name))
                        return {"script", n};
                return {"builtin", std::nullopt};
            }

            const T6Api& api;
            std::vector<std::string> order;
            std::vector<fs::path> paths;
            std::vector<Script> generated;
            std::unordered_map<std::string, size_t> index;
        };
    } // namespace

    PortReport PortMap(const Sources& input, const T6Api& api, const std::string& mapName, const fs::path& outRoot, const PortMapOptions& options)
    {
        try
        {
            return PortMapImpl(input, api, mapName, outRoot, options);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(g_phase + ": " + error.what());
        }
    }

    int FixArity(const fs::path& outRoot, const T6Api& api, PortReport* report)
    {
        Linker linker(outRoot, api);
        int fixed = 0;
        for (size_t k = 0; k < linker.generated.size(); k++)
        {
            auto& script = linker.generated[k];
            bool changed = false;
            std::set<size_t> defs;
            for (const auto& [_, fn] : script.functions)
                defs.insert(fn.start);
            for (const auto& ref : References(script.tokens))
            {
                if (defs.count(ref.index) || ref.pointer || script.tokens[ref.index].text.empty())
                    continue;
                const auto [kind, n] = linker.Target(script, ref);
                const auto argc = Argc(script.tokens, static_cast<long long>(ref.index));
                if (kind == "script" && argc && n && *argc > *n)
                {
                    TruncateArgs(script.tokens, static_cast<long long>(ref.index), *n);
                    changed = true;
                    fixed++;
                    if (report)
                        report->rewrites["call " + ref.name + ": " + std::to_string(*argc - *n) + " argument(s) beyond its parameters dropped"] += 1;
                }
            }
            if (changed)
                files::WriteText(linker.paths[k], Emit(script.tokens));
        }
        return fixed;
    }
} // namespace w2b::gsc::port

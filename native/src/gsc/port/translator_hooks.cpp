// Port of the Translator hook generators (gscport.py 2881-3642).
#include "translator.h"

#include "api.h"
#include "core/files.h"
#include "core/pyre.h"
#include "rewrites.h"

#include <algorithm>

namespace w2b::gsc::port
{
    namespace
    {
        Tokens SliceCopy(const Tokens& tokens, const long long a, const long long b)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            return Tokens(tokens.begin() + static_cast<long long>(lo), tokens.begin() + static_cast<long long>(hi));
        }

        std::string Origin(Sources& sources, const std::string& key, const std::string& fallback)
        {
            const auto* found = sources.origin->Find(key);
            return found ? *found : fallback;
        }

        std::string Prefix(const std::string& path)
        {
            const auto slash = path.rfind('\\');
            auto last = slash == std::string::npos ? path : path.substr(slash + 1);
            return last.substr(std::min(last.find_first_not_of('_'), last.size()));
        }

        // str() of a JSON number in an f-string
        std::string Number(const py::Json& value)
        {
            if (value.is_number_float())
                return py::FloatRepr(value.get<double>());
            if (value.is_string())
                return value.get<std::string>();
            return value.dump();
        }

        bool Truthy(const py::Json& value)
        {
            if (value.is_null())
                return false;
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_number())
                return value.get<double>() != 0.0;
            if (value.is_string())
                return !value.get_ref<const std::string&>().empty();
            return !value.empty();
        }

        std::vector<std::string> SortedKeys(const TextDict& dict, const std::string& suffix)
        {
            std::vector<std::string> keys;
            for (const auto& k : dict.order)
                if (EndsWith(k, suffix))
                    keys.push_back(k);
            std::sort(keys.begin(), keys.end());
            return keys;
        }
    } // namespace

    std::vector<std::string> Translator::JuggernogHooks()
    {
        std::vector<std::string> calls;
        const auto* perksText = sources.text->Find(std::string(PERKS) + ".gsc");
        static const pyre::Regex maxhealth(R"(\bplayer\.maxhealth\s*=\s*(\d+)\s*;)", pyre::IGNORECASE);
        std::vector<long long> health;
        if (perksText)
            for (const auto& row : maxhealth.FindAll(*perksText))
                if (std::stoll(row[0]) > 100)
                    health.push_back(std::stoll(row[0]));
        if (!health.empty())
        {
            calls.push_back("level.zombie_vars[\"zombie_perk_juggernaut_health\"] = " + std::to_string(health[0]) + ";");
            calls.push_back("level.zombie_vars[\"zombie_perk_juggernaut_health_upgrade\"] = " + std::to_string(health[0]) + ";");
            report.rewrites["WaW Juggernog health -> BO2 zombie_perk_juggernaut_health"] += 1;
        }
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* main = script ? script->Find("main") : nullptr;
        if (main && script->Find("check_for_jugg_perk"))
        {
            bool called = false;
            for (const auto& r : References(script->tokens, main->bodyOpen, main->bodyClose))
                called = called || Lower(r.name) == "check_for_jugg_perk";
            if (called)
            {
                const auto name = ExtractEntry(ZOMBIEMODE, "check_for_jugg_perk");
                calls.push_back("level thread " + std::string(CORE) + "::" + name.value_or("None") + "();");
                report.rewrites["WaW check_for_jugg_perk regen -> started after _zm::init"] += 1;
            }
        }
        return calls;
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> Translator::AnnouncerLines(const std::string& core, const std::string& function, const int depth)
    {
        const auto script = sources.Get(core);
        const Function* fn = script ? script->Find(function) : nullptr;
        if (!fn)
            return {};
        const auto& toks = script->tokens;
        std::vector<std::pair<std::string, std::vector<std::string>>> lines;
        std::function<std::vector<std::string>(long long, long long, const std::set<std::string>&, int)> literals;
        literals = [&](const long long open, const long long close, const std::set<std::string>& seen, const int level)
        {
            std::vector<std::string> found;
            for (const auto& ref : References(toks, static_cast<size_t>(open), static_cast<size_t>(close)))
            {
                const auto i = static_cast<long long>(ref.index);
                const auto callee = Lower(ref.name);
                if (i + 3 >= static_cast<long long>(toks.size()) || At(toks, i + 1).text != "(")
                    continue;
                const auto owner = FunctionScript(core, ref);
                if (!owner)
                    continue;
                if (At(toks, i + 2).kind == Kind::String && In(At(toks, i + 3).text, {",", ")"}) && PlaysArgumentSound(*owner, callee))
                    found.push_back(Lower(StripChars(At(toks, i + 2).text, "\"")));
                else if (*owner == core && level < depth && !seen.count(callee))
                {
                    const Function& inner = *script->Find(callee);
                    auto next = seen;
                    next.insert(callee);
                    const auto more = literals(static_cast<long long>(inner.bodyOpen), static_cast<long long>(inner.bodyClose), next, level + 1);
                    found.insert(found.end(), more.begin(), more.end());
                }
            }
            return found;
        };
        for (long long i = static_cast<long long>(fn->bodyOpen); i < static_cast<long long>(fn->bodyClose); i++)
        {
            if (At(toks, i).Low() != "switch")
                continue;
            const auto close = static_cast<long long>(Match(toks, Match(toks, static_cast<size_t>(i + 1), "(", ")") + 1, "{", "}"));
            std::vector<long long> cases;
            for (long long j = i; j < close; j++)
                if (In(At(toks, j).Low(), {"case", "default"}))
                    cases.push_back(j);
            for (size_t k = 0; k < cases.size(); k++)
            {
                const auto j = cases[k];
                if (At(toks, j).Low() != "case" || At(toks, j + 1).kind != Kind::String)
                    continue;
                const auto end = k + 1 < cases.size() ? cases[k + 1] : close;
                const auto label = StripChars(At(toks, j + 1).text, "\"");
                const auto aliases = literals(j, end, {function}, 1);
                if (aliases.empty())
                    continue;
                std::vector<std::string> unique;
                for (const auto& a : aliases)
                    if (std::find(unique.begin(), unique.end(), a) == unique.end())
                        unique.push_back(a);
                const auto it = std::find_if(lines.begin(), lines.end(), [&](const auto& l) { return l.first == label; });
                if (it != lines.end())
                    it->second = unique;
                else
                    lines.emplace_back(label, unique);
            }
        }
        return lines;
    }

    std::vector<std::string> Translator::PowerupModelsHook()
    {
        const auto& contract = Api()["map_content"]["powerup_models"];
        const auto script = sources.Get(contract["script"].get<std::string>());
        if (!script)
            return {};
        const auto& toks = script->tokens;
        std::map<std::string, std::string> models;
        for (const auto& ref : References(toks, 0, toks.size()))
        {
            const auto i = static_cast<long long>(ref.index);
            if (Lower(ref.name) != contract["register"].get<std::string>() || i + 5 >= static_cast<long long>(toks.size()) || At(toks, i + 1).text != "(")
                continue;
            if (At(toks, i + 2).kind == Kind::String && At(toks, i + 3).text == "," && At(toks, i + 4).kind == Kind::String)
                models[StripChars(At(toks, i + 2).text, "\"")] = StripChars(At(toks, i + 4).text, "\"");
        }
        if (models.empty())
            return {};
        std::string body;
        for (const auto& [name, model] : models)
            body += std::string("    ") + COMPAT + "::waw_precachemodel( \"" + model + "\" );\n    if ( isdefined( level.zombie_powerups ) && isdefined( level.zombie_powerups[\"" + name
                    + "\"] ) )\n        level.zombie_powerups[\"" + name + "\"].model_name = \"" + model + "\";\n";
        levelStateParts.push_back("\n// " + contract["script"].get<std::string>() + ": the map's power-up models on BO2's power-up registry\nwaw2bo2_powerup_models()\n{\n" + body
                                  + "}\n");
        report.rewrites["WaW power-up models -> BO2 power-up registry"] += static_cast<long long>(models.size());
        return {std::string(CORE) + "::waw2bo2_powerup_models();"};
    }

    std::vector<std::string> Translator::NativeReplacements()
    {
        std::vector<std::string> calls, parts;
        for (const auto& [core, entries] : Api()["native_replacements"].items())
        {
            if (core == "doc")
                continue;
            for (const auto& entry : entries)
            {
                const auto native = entry["native"].get<std::string>();
                const auto cut = native.rfind("::");
                const auto nativeScript = native.substr(0, cut), nativeName = native.substr(cut + 2);
                const auto script = sources.Get(core);
                const auto function = entry["function"].get<std::string>();
                const Function* fn = script ? script->Find(function) : nullptr;
                const auto scriptIt = api.scriptIndex.find(Lower(nativeScript));
                std::optional<int> arity;
                if (scriptIt != api.scriptIndex.end())
                {
                    const auto& functions = api.scripts[scriptIt->second].second;
                    if (const auto f = functions.find(nativeName); f != functions.end())
                        arity = f->second;
                }
                if (!fn || !arity)
                    continue;
                const bool preserved = ApiListHas(Api().value("preserved_scripts", py::Json::array()), Norm(core));
                const auto name = preserved ? std::optional<std::string>(function) : ExtractEntry(core, function);
                const auto target = preserved ? PortedPath(core) : std::string(CORE);
                std::vector<std::string> params;
                for (int i = 0; i < *arity; i++)
                    params.push_back("arg" + std::to_string(i));
                const std::vector<std::string> passedParams(params.begin(), params.begin() + static_cast<long long>(std::min(params.size(), fn->params.size())));
                const auto wrapper = "waw2bo2_native_" + Prefix(nativeScript) + "_" + nativeName;
                std::string keep;
                if (entry.contains("keep"))
                    for (const auto& k : entry["keep"])
                        keep += "    // " + k["reason"].get<std::string>() + "\n    " + k["call"].get<std::string>() + ";\n";
                parts.push_back("// " + core + "::" + function + " in place of " + native + "\n" + wrapper + "( " + Join(params, ", ") + " )\n{\n" + keep
                                + "    return self " + target + "::" + name.value_or("None") + "( " + Join(passedParams, ", ") + " );\n}\n");
                calls.push_back("replacefunc( " + native + ", " + std::string(CORE) + "::" + wrapper + " );");
            }
        }
        if (!parts.empty())
        {
            levelStateParts.push_back("\n" + Join(parts, "\n"));
            report.rewrites["WaW functions in place of BO2's (replacefunc)"] += static_cast<long long>(parts.size());
        }
        return calls;
    }

    std::vector<std::string> Translator::QuoteHooks()
    {
        const auto& contract = Api()["quote_triggers"];
        std::vector<std::string> parts, calls;
        for (const auto& entry : contract["replace"])
        {
            const auto native = entry["native"].get<std::string>();
            const auto cut = native.rfind("::");
            const auto name = api.Defines(native.substr(0, cut), native.substr(cut + 2))
                                  ? ExtractEntry(entry["script"].get<std::string>(), entry["function"].get<std::string>())
                                  : std::nullopt;
            if (name && !name->empty())
                calls.push_back("    // " + entry["script"].get<std::string>() + "::" + entry["function"].get<std::string>() + " at BO2's call\n    replacefunc( " + native + ", "
                                + CORE + "::" + *name + " );\n");
        }
        const auto& death = contract["death_event"];
        if (const auto name = ExtractEntry(death["script"].get<std::string>(), death["function"].get<std::string>()); name && !name->empty())
            parts.push_back("// " + death["script"].get<std::string>() + ": zombie_spawn_init threads it on each zombie\nwaw2bo2_zombie_death_event( zombie )\n{\n    level thread "
                            + CORE + "::" + *name + "( zombie );\n}\n");
        const auto& ammo = contract["ammo"];
        if (const auto name = ExtractEntry(ammo["script"].get<std::string>(), ammo["function"].get<std::string>()); name && !name->empty())
            calls.push_back("    level thread " + std::string(CORE) + "::" + *name + "();\n");
        if (parts.empty() && calls.empty())
            return {};
        std::string joinedCalls;
        for (const auto& c : calls)
            joinedCalls += c;
        levelStateParts.push_back("\n" + Join(parts, "\n") + "\n// the map's quote triggers on BO2's events\nwaw2bo2_quote_triggers()\n{\n" + joinedCalls + "}\n");
        report.rewrites["WaW quote triggers -> BO2 events"] += static_cast<long long>(parts.size() + calls.size());
        return {std::string(CORE) + "::waw2bo2_quote_triggers();"};
    }

    std::vector<std::string> Translator::ZombieVocalsHook()
    {
        std::string body = "    if ( !isdefined( level.zmb_vox ) || !isdefined( level.zmb_vox[\"prefix\"] ) )\n        return;\n"
                           "    prefix = level.zmb_vox[\"prefix\"];\n"
                           "    foreach ( type, entries in level.zmb_vox )\n    {\n"
                           "        if ( type == \"prefix\" )\n            continue;\n"
                           "        foreach ( category, alias in entries )\n"
                           "            level.zmb_vox[type][category] = prefix + alias;\n    }\n"
                           "    level.zmb_vox[\"prefix\"] = \"\";\n";
        for (const auto& [actor, categories] : Api()["zombie_vocals"].items())
        {
            if (actor == "doc")
                continue;
            body += "    if ( !isdefined( level.zmb_vox[\"" + actor + "\"] ) )\n        level.zmb_vox[\"" + actor + "\"] = [];\n";
            for (const auto& [category, alias] : categories.items())
                body += "    if ( soundexists( \"waw/" + alias.get<std::string>() + "\" ) )\n        level.zmb_vox[\"" + actor + "\"][\"" + category + "\"] = \"waw/"
                        + alias.get<std::string>() + "\";\n";
        }
        levelStateParts.push_back("\n// BO2's zombie vocal events play the map's own voices\nwaw2bo2_zombie_vocals()\n{\n" + body + "}\n");
        report.rewrites["WaW zombie vocals -> BO2 zmb_vox"] += 1;
        return {std::string(CORE) + "::waw2bo2_zombie_vocals();"};
    }

    std::vector<std::string> Translator::SoundTableHook()
    {
        const auto& contract = Api()["map_content"]["sound_table"];
        const auto framework = Norm(contract["script"].get<std::string>()) + ".gsc";
        std::vector<std::string> keys{framework};
        for (const auto& k : SortedKeys(*sources.text, ".gsc"))
            if (k != framework)
                keys.push_back(k);
        OrderedMap<std::string> table;
        for (const auto& key : keys)
        {
            const auto script = sources.Get(key.substr(0, key.size() - 4));
            if (!script)
                continue;
            const auto& toks = script->tokens;
            for (const auto& ref : References(toks, 0, toks.size()))
            {
                const auto i = static_cast<long long>(ref.index);
                if (Lower(ref.name) != contract["register"].get<std::string>() || i + 5 >= static_cast<long long>(toks.size()) || At(toks, i + 1).text != "(")
                    continue;
                if (At(toks, i + 2).kind == Kind::String && At(toks, i + 3).text == "," && At(toks, i + 4).kind == Kind::String && At(toks, i + 5).text == ")")
                    table[StripChars(At(toks, i + 2).text, "\"")] = StripChars(At(toks, i + 4).text, "\"");
            }
        }
        if (table.order.empty())
            return {};
        std::string body;
        for (const auto& name : table.order)
            body += "    if ( soundexists( \"waw/" + table.values[name] + "\" ) )\n        level.zombie_sounds[\"" + name + "\"] = \"waw/" + table.values[name] + "\";\n";
        levelStateParts.push_back("\n// " + contract["script"].get<std::string>() + "::init_sounds: the map's sound for each ref on BO2's sound table\nwaw2bo2_sound_table()\n{\n"
                                  "    if ( !isdefined( level.zombie_sounds ) )\n        level.zombie_sounds = [];\n"
                                  + body + "}\n");
        report.rewrites["WaW sound table -> BO2 level.zombie_sounds"] += static_cast<long long>(table.order.size());
        return {std::string(CORE) + "::waw2bo2_sound_table();"};
    }

    std::vector<std::string> Translator::AnnouncerHook(const std::filesystem::path& outRoot)
    {
        const auto& contract = Api()["audio_remaster"]["powerup_announcer"];
        auto lines = AnnouncerLines(contract["script"].get<std::string>(), contract["function"].get<std::string>());
        const auto events = outRoot / "content_source" / "announcer_events.json";
        std::error_code error;
        std::filesystem::remove(events, error);
        if (lines.empty())
            return {};
        py::Json json = py::Json::object();
        for (const auto& [label, aliases] : lines)
            json[label] = aliases;
        files::WriteText(events, py::JsonDumps(json, 2) + "\n");
        const auto prefix = Api()["audio_remaster"]["announcer_alias_prefix"].get<std::string>();
        static const pyre::Regex nonword(R"(\W)");
        std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string body;
        for (const auto& [label, aliases] : lines)
            for (const auto& alias : aliases)
            {
                const auto name = prefix + nonword.Sub("_", alias);
                body += "    if ( !isdefined( bound[\"" + label + "\"] ) && soundexists( \"vox_zmba_" + name + "\" ) )\n    {\n        game[\"zmbdialog\"][\"" + label + "\"] = \"" + name
                        + "\";\n        bound[\"" + label + "\"] = 1;\n    }\n";
            }
        levelStateParts.push_back("\n// " + contract["script"].get<std::string>() + "::" + contract["function"].get<std::string>()
                                  + ": WaW's announcer line for each power-up, said by BO2's announcer\nwaw2bo2_announcer_events()\n{\n"
                                    "    if ( !isdefined( game[\"zmbdialog\"] ) )\n        return;\n    bound = [];\n"
                                  + body + "}\n");
        report.rewrites["WaW power-up announcer lines -> BO2 announcer events"] += static_cast<long long>(lines.size());
        return {std::string(CORE) + "::waw2bo2_announcer_events();"};
    }

    std::vector<std::string> Translator::SkillSignalsHook()
    {
        if (!sources.Get(GAMESKILL))
            return {};
        const auto name = ExtractEntry(LOAD, "all_players_spawned");
        if (!name)
        {
            report.Unsupported(FALLBACK, std::string(LOAD) + ": no all_players_spawned source; " + GAMESKILL + " co-op scaling never runs");
            return {};
        }
        levelStateParts.push_back("\n// maps\\_load: the all_players_spawned flag _gameskill's co-op scaling waits for\nwaw2bo2_all_players_spawned()\n{\n"
                                  "    if ( !isdefined( level.flag ) || !isdefined( level.flag[\"all_players_spawned\"] ) )\n"
                                  "        common_scripts\\utility::flag_init( \"all_players_spawned\" );\n    level thread "
                                  + std::string(CORE) + "::" + *name + "();\n}\n");
        report.rewrites["WaW _load all_players_spawned -> source"] += 1;
        return {std::string(CORE) + "::waw2bo2_all_players_spawned();"};
    }

    std::vector<std::string> Translator::ExplodersHook()
    {
        const auto name = ExtractEntry(LOAD, "setupexploders");
        if (!name)
        {
            report.Unsupported(FALLBACK, std::string(LOAD) + ": no setupExploders source; client exploders never start");
            return {};
        }
        report.rewrites["WaW _load setupExploders -> source"] += 1;
        return {std::string(CORE) + "::" + *name + "();"};
    }

    std::optional<std::string> Translator::DogRoundsHook()
    {
        const auto& dogs = Api()["zombie_runtime"]["dog_rounds"];
        const auto source = dogs["source_script"].get<std::string>(), native = dogs["native_script"].get<std::string>();
        const auto script = sources.Get(source);
        if (!script)
            return std::nullopt;
        std::string logic = "undefined";
        const auto spawningName = dogs["source_spawning"].get<std::string>();
        if (const Function* spawning = script->Find(spawningName))
        {
            std::set<std::string> called;
            for (const auto& r : References(script->tokens, spawning->bodyOpen, spawning->bodyClose))
                called.insert(Lower(r.name));
            std::optional<std::string> chosen;
            for (const auto& n : dogs["spawn_logic"])
                if (!chosen && called.count(n.get<std::string>()) && api.Defines(native, n.get<std::string>()))
                    chosen = n.get<std::string>();
            bool local = false;
            for (const auto& [fname, _] : script->functions)
                local = local || (fname != spawningName && called.count(fname));
            if (chosen)
                logic = dogs["owned_script"].get<std::string>() + "::" + *chosen;
            else if (local)
                report.Unsupported("GSC " + source + "::" + spawningName + " spawn selection", Origin(sources, source + ".gsc", source));
        }
        std::string health = "undefined";
        if (script->Find(dogs["health"].get<std::string>()))
            health = std::string(CORE) + "::" + ExtractEntry(source, dogs["health"].get<std::string>()).value_or("None");
        const auto fog = ClientFogMonitors(dogs["client_notifies"]);
        static const pyre::Regex loadfx(R"re(\bloadfx\s*\(\s*"([^"]+)")re", pyre::IGNORECASE);
        if (const auto* text = sources.text->Find(source + ".gsc"))
            for (const auto& row : loadfx.FindAll(*text))
                report.fx.push_back(row[0]);
        report.rewrites["WaW dog rounds -> " + native + " (source spawn selection/health)"] += 1;
        const auto guard = "level." + dogs["guard"].get<std::string>();
        std::string threads;
        for (const auto& name : fog)
            threads += " level thread " + std::string(CORE) + "::" + name + "();";
        const auto owned = dogs["owned_script"].get<std::string>();
        return "if ( isdefined( " + guard + " ) && " + guard + " ) { " + owned + "::" + dogs["native_init"].get<std::string>() + "(); " + COMPAT + "::waw_dog_rounds( " + logic
               + ", " + health + ", " + owned + "::enable_dog_rounds );" + threads + " }";
    }

    std::vector<std::string> Translator::ClientFogMonitors(const py::Json& moments)
    {
        std::vector<std::string> made;
        for (const auto& key : SortedKeys(*sources.text, ".csc"))
        {
            const auto script = sources.Get(key.substr(0, key.size() - 4), ".csc");
            if (!script)
                continue;
            const auto& toks = script->tokens;
            for (const auto& [_, fn] : script->functions)
            {
                for (long long i = static_cast<long long>(fn.bodyOpen); i < static_cast<long long>(fn.bodyClose) - 4; i++)
                {
                    if (!(At(toks, i).Low() == "level" && At(toks, i + 1).Low() == "waittill" && At(toks, i + 2).text == "(" && At(toks, i + 3).kind == Kind::String))
                        continue;
                    const auto msg = StripChars(At(toks, i + 3).text, "\"");
                    if (!moments.contains(msg))
                        continue;
                    const auto close = static_cast<long long>(Match(toks, static_cast<size_t>(i + 2), "(", ")"));
                    long long blockEnd = static_cast<long long>(fn.bodyClose);
                    for (long long j = close; j < static_cast<long long>(fn.bodyClose); j++)
                        if (At(toks, j).text == "}")
                        {
                            blockEnd = j;
                            break;
                        }
                    std::vector<std::string> fogs;
                    for (const auto& ref : References(toks, static_cast<size_t>(close), static_cast<size_t>(blockEnd)))
                    {
                        if (Lower(ref.name) == "setvolfog" && !ref.method)
                        {
                            const auto end = static_cast<long long>(Match(toks, ref.index + 1, "(", ")"));
                            bool literal = true;
                            for (long long k = static_cast<long long>(ref.index) + 2; k < end; k++)
                                literal = literal && (At(toks, k).kind == Kind::Number || In(At(toks, k).text, {",", "-", "."}));
                            if (literal)
                                fogs.push_back(PyStrip(EmitRange(toks, static_cast<long long>(ref.index) + 1, end + 1)));
                        }
                        else if (!ref.qualifier && !ref.method)
                            report.Unsupported("CSC " + ref.name + " in a " + msg + " monitor (client-only)", key);
                    }
                    if (fogs.empty())
                        continue;
                    const auto& list = moments[msg];
                    for (size_t n = 0; n < list.size(); n++)
                    {
                        const auto name = "waw2bo2_client_" + msg + "_" + std::to_string(n);
                        bool exists = false;
                        for (const auto& part : levelStateParts)
                            exists = exists || StartsWith(part, name + "()");
                        if (exists)
                            continue;
                        std::string body;
                        for (const auto& args : fogs)
                            body += std::string("\t\t") + COMPAT + "::waw_setvolfog" + args + ";\n";
                        const auto& delay = list[n][1];
                        const std::string wait = Truthy(delay) ? "\t\twait " + Number(delay) + ";\n" : "";
                        levelStateParts.push_back(name + "()\n{\n\t// " + key + "::" + fn.name + ": client " + msg + " fog, on the server\n"
                                                  + "\tlevel endon( \"end_game\" );\n\tfor ( ;; )\n\t{\n\t\tlevel waittill( \"" + list[n][0].get<std::string>() + "\" );\n" + wait
                                                  + body + "\t}\n}\n");
                        made.push_back(name);
                        report.rewrites["client " + msg + " fog monitor -> server"] += 1;
                    }
                }
            }
        }
        return made;
    }

    std::optional<std::string> Translator::FrameworkZombieVars()
    {
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* fn = script ? script->Find("init_levelvars") : nullptr;
        if (!fn && script)
            fn = script->Find("init_zombie_vars");
        if (!fn)
            return std::nullopt;
        const auto& toks = script->tokens;
        std::map<std::string, std::string> values, divisors;
        bool dev = false;
        for (long long i = static_cast<long long>(fn->bodyOpen); i < static_cast<long long>(fn->bodyClose) - 5; i++)
        {
            if (In(At(toks, i).text, {"/#", "#/"}))
                dev = At(toks, i).text == "/#";
            if (dev)
                continue;
            if (At(toks, i).Low() == "set_zombie_var" && At(toks, i + 1).text == "(" && At(toks, i + 2).kind == Kind::String && At(toks, i + 3).text == ",")
            {
                long long j = i + 4;
                std::string sign;
                if (At(toks, j).text == "-")
                {
                    sign = "-";
                    j++;
                }
                const auto& name = At(toks, i + 2).text;
                const auto inner = name.size() >= 2 ? name.substr(1, name.size() - 2) : std::string();
                if ((At(toks, j).kind == Kind::Number || In(At(toks, j).Low(), {"true", "false"})) && In(At(toks, j + 1).text, {",", ")"}))
                {
                    values[inner] = sign + At(toks, j).text;
                    if (At(toks, j + 1).text == "," && At(toks, j + 2).kind == Kind::Number && In(At(toks, j + 3).text, {",", ")"}))
                        divisors[inner] = At(toks, j + 2).text;
                }
            }
        }
        if (values.empty())
            return std::nullopt;
        const std::string name = "zombiemode__init_zombie_vars_defaults";
        std::string text = "\n// " + std::string(ZOMBIEMODE) + "::init_zombie_vars: WaW defaults for BO2's init_zombie_vars\n" + name + "()\n{\n"
                           + "\tlevel.waw2bo2_zombie_var_defaults = [];\n";
        for (const auto& [k, v] : values)
            text += "\tlevel.waw2bo2_zombie_var_defaults[\"" + k + "\"] = " + v + ";\n";
        text += "\tlevel.waw2bo2_zombie_var_divisors = [];\n";
        for (const auto& [k, v] : divisors)
            text += "\tlevel.waw2bo2_zombie_var_divisors[\"" + k + "\"] = " + v + ";\n";
        levelStateParts.push_back(text + "}\n");
        report.rewrites["WaW framework zombie_vars defaults"] += static_cast<long long>(values.size());
        return name;
    }

    std::vector<std::string> Translator::ClientFxListeners()
    {
        std::vector<std::string> made;
        std::map<std::string, std::string> effects;
        const auto csc = SortedKeys(*sources.text, ".csc");
        for (const auto& key : csc)
        {
            const auto script = sources.Get(key.substr(0, key.size() - 4), ".csc");
            if (!script)
                continue;
            const auto& toks = script->tokens;
            for (long long i = 0; i < static_cast<long long>(toks.size()) - 9; i++)
                if (WordsEqual(toks, i, {"level", ".", "_effect", "["}) && At(toks, i + 4).kind == Kind::String && At(toks, i + 5).text == "]"
                    && At(toks, i + 6).text == "=" && At(toks, i + 7).Low() == "loadfx" && At(toks, i + 9).kind == Kind::String)
                {
                    const auto& t = At(toks, i + 4).text;
                    effects.emplace(Lower(t.size() >= 2 ? t.substr(1, t.size() - 2) : std::string()), At(toks, i + 9).text);
                }
        }
        for (const auto& key : csc)
        {
            const auto script = sources.Get(key.substr(0, key.size() - 4), ".csc");
            const Function* main = script ? script->Find("main") : nullptr;
            if (!main)
                continue;
            const auto& toks = script->tokens;
            for (const auto& ref : References(toks, main->bodyOpen, main->bodyClose))
            {
                if (ref.qualifier || ref.pointer || ref.method || !script->Find(Lower(ref.name)))
                    continue;
                const auto i = static_cast<long long>(ref.index);
                if (!(At(toks, i - 1).Low() == "thread" && At(toks, i - 2).Low() == "level"))
                    continue;
                const auto listener = ClientFxListener(key, *script, Lower(ref.name), effects);
                if (!listener)
                    continue;
                const auto end = static_cast<long long>(Match(toks, ref.index + 1, "(", ")"));
                const auto args = PyStrip(EmitRange(toks, i + 1, end + 1));
                const auto name = "waw2bo2_client_fx_" + std::to_string(made.size());
                levelStateParts.push_back(name + "()\n{\n\t// " + key + "::main: client listener, on the server\n" + listener->effects + "\tlevel thread " + listener->entry + args
                                          + ";\n}\n");
                made.push_back(name);
                report.rewrites["client effect listeners -> server"] += 1;
                std::set<std::string> strings(report.clientListenerStrings.begin(), report.clientListenerStrings.end());
                for (const auto& t : Tokenize(listener->code + args))
                    if (t.kind == Kind::String)
                        strings.insert(t.text.size() >= 2 ? t.text.substr(1, t.text.size() - 2) : std::string());
                report.clientListenerStrings.assign(strings.begin(), strings.end());
            }
        }
        return made;
    }

    std::optional<ListenerCopy> Translator::ClientFxListener(const std::string& key, const Script& script, const std::string& entry,
                                                             const std::map<std::string, std::string>& effects)
    {
        const std::pair<std::string, std::string> id{key, entry};
        if (!listenerCopies.count(id))
            listenerCopies[id] = ClientFxListenerCopy(key, script, entry, effects);
        return listenerCopies[id];
    }

    void Translator::DropArgument(Tokens& toks, const long long nameIndex, const size_t index)
    {
        const auto close = static_cast<long long>(Match(toks, static_cast<size_t>(nameIndex + 1), "(", ")"));
        int depth = 0;
        std::vector<long long> commas;
        for (long long k = nameIndex + 2; k < close; k++)
        {
            if (At(toks, k).text == "," && depth == 0)
                commas.push_back(k);
            depth += In(At(toks, k).text, {"(", "["});
            depth -= In(At(toks, k).text, {")", "]"});
        }
        std::vector<long long> bounds{nameIndex + 1};
        bounds.insert(bounds.end(), commas.begin(), commas.end());
        bounds.push_back(close);
        if (index >= bounds.size() - 1)
            return;
        long long first, last;
        if (index + 1 < bounds.size() - 1)
        {
            first = bounds[index] + 1;
            last = bounds[index + 1];
        }
        else
        {
            first = index ? bounds[index] : bounds[index] + 1;
            last = bounds[index + 1] - 1;
        }
        for (long long k = first; k < last + 1; k++)
            At(toks, k).text = At(toks, k).pre = "";
    }

    std::optional<ListenerCopy> Translator::ClientFxListenerCopy(const std::string& key, const Script& script, const std::string& entry,
                                                                 const std::map<std::string, std::string>& effects)
    {
        const auto& calls = Api()["client_listener_calls"];
        std::vector<std::string> names, pending{entry};
        while (!pending.empty())
        {
            const auto name = pending.back();
            pending.pop_back();
            if (std::find(names.begin(), names.end(), name) != names.end())
                continue;
            names.push_back(name);
            const Function& fn = *script.Find(name);
            for (const auto& t : SliceCopy(script.tokens, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
                if (t.text == "[[")
                {
                    report.Unsupported("CSC function pointer call in client listener " + entry + " (client-only)", key);
                    return std::nullopt;
                }
            for (const auto& ref : References(script.tokens, fn.bodyOpen, fn.bodyClose))
            {
                const auto low = Lower(ref.name);
                const py::Json* rule = !ref.qualifier ? ApiGet(calls, low) : nullptr;
                if (!ref.qualifier && script.Find(low))
                    pending.push_back(low);
                else if (rule && rule->contains("skip") && Truthy((*rule)["skip"]))
                    return std::nullopt;
                else if (!rule || (ref.method && rule->contains("to")))
                {
                    report.Unsupported("CSC " + ref.name + " in client listener " + entry + " (client-only)", key);
                    return std::nullopt;
                }
            }
        }
        bool waits = false;
        for (const auto& n : names)
        {
            const Function& fn = *script.Find(n);
            for (long long i = static_cast<long long>(fn.bodyOpen); i < static_cast<long long>(fn.bodyClose); i++)
                waits = waits || (At(script.tokens, i).Low() == "level" && At(script.tokens, i + 1).Low() == "waittill");
        }
        if (!waits)
            return std::nullopt;
        auto base = key.substr(key.rfind('\\') == std::string::npos ? 0 : key.rfind('\\') + 1);
        if (EndsWith(base, ".csc"))
            base.resize(base.size() - 4);
        const auto prefix = "waw2bo2_" + base + "__";
        std::vector<std::pair<std::string, std::string>> parts;
        std::set<std::string> used;
        for (const auto& name : names)
        {
            const Function& fn = *script.Find(name);
            if (listenerFunctions.count({key, name}))
                continue;
            Tokens toks;
            for (const auto& t : SliceCopy(script.tokens, static_cast<long long>(fn.start), static_cast<long long>(fn.bodyClose) + 1))
                toks.push_back(Token{t.kind, t.text, t.pre, t.line});
            long long i = 0;
            while (i < static_cast<long long>(toks.size()) - 1)
            {
                auto& t = At(toks, i);
                const auto low = t.Low();
                const bool call = t.kind == Kind::Ident && At(toks, i + 1).text == "(" && (i == 0 || At(toks, i - 1).text != "::");
                const py::Json* rule = call && i ? ApiGet(calls, low) : nullptr;
                if (call && i && std::find(names.begin(), names.end(), low) != names.end())
                    t.text = prefix + low;
                else if (rule)
                {
                    if (rule->contains("client_arg"))
                        DropArgument(toks, i, (*rule)["client_arg"].get<size_t>());
                    if (low == "spawnfx")
                        DropArgument(toks, i, 2);
                    if (rule->contains("to"))
                    {
                        const auto to = (*rule)["to"].get<std::string>();
                        At(toks, i).text = StartsWith(to, "waw_") ? std::string(COMPAT) + "::" + to : to;
                    }
                }
                else if (low == "level" && At(toks, i + 1).text == "." && At(toks, i + 2).Low() == "_effect" && At(toks, i + 3).text == "["
                         && At(toks, i + 4).kind == Kind::String)
                {
                    const auto& text = At(toks, i + 4).text;
                    used.insert(Lower(text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string()));
                    At(toks, i + 2).text = "waw_effect";
                }
                else if (low == "level" && In(At(toks, i + 1).Low(), {"waittill", "notify", "endon"}) && At(toks, i + 2).text == "(")
                    At(toks, i + 3).pre = " \"waw_client_\" + " + PyLStrip(At(toks, i + 3).pre);
                i++;
            }
            if (std::find(names.begin(), names.end(), toks[0].Low()) != names.end())
                toks[0].text = prefix + toks[0].Low();
            parts.emplace_back(name, Emit(toks));
        }
        std::vector<std::string> missing;
        for (const auto& u : used)
            if (!effects.count(u))
                missing.push_back(u);
        if (!missing.empty())
        {
            report.Unsupported("CSC client effects " + Join(missing, ", ") + " in listener " + entry + " (no client registration)", key);
            return std::nullopt;
        }
        std::set<std::string> done;
        for (const auto& [n, _] : parts)
            done.insert(n);
        for (const auto& n : done)
            listenerFunctions.insert({key, n});
        std::vector<std::string> texts;
        for (const auto& [_, text] : parts)
            texts.push_back(text);
        auto code = Join(texts, "\n");
        levelStateParts.push_back("\n// " + key + ": client listener " + entry + " (server copy)\n" + code + "\n");
        std::string registrations;
        for (const auto& k : used)
            registrations += "\tif ( !isdefined( level.waw_effect[\"" + k + "\"] ) )\n\t\tlevel.waw_effect[\"" + k + "\"] = " + COMPAT + "::waw_loadfx( " + effects.at(k) + " );\n";
        for (const auto& n : names)
            if (!done.count(n))
            {
                const Function& fn = *script.Find(n);
                code += EmitRange(script.tokens, static_cast<long long>(fn.start), static_cast<long long>(fn.bodyClose) + 1);
            }
        return ListenerCopy{prefix + entry, registrations, code};
    }

    std::vector<std::string> Translator::FrameworkHooks()
    {
        const auto zm = sources.Get(ZOMBIEMODE);
        const Function* fnp = zm ? zm->Find("main") : nullptr;
        if (!fnp)
            return {};
        const Function fn = *fnp;
        const auto whereFile = Origin(sources, std::string(ZOMBIEMODE) + ".gsc", ZOMBIEMODE);
        const auto& A = Api();
        std::vector<std::string> calls;
        for (const auto& ref : References(zm->tokens, fn.bodyOpen, fn.bodyClose))
        {
            if (!ref.qualifier || ref.pointer || ref.method)
                continue;
            if (!ScriptBranchTaken(zm->tokens, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose), static_cast<long long>(ref.index), mapName))
                continue;
            const auto target = Norm(*ref.qualifier);
            const auto low = Lower(ref.name);
            const auto& tesla = A["tesla_runtime"];
            if (target == tesla["source_script"].get<std::string>() && low == tesla["initialize"].get<std::string>() && coreFuncs.count({target, low}))
                continue;
            if (bo2StockPerks && StartsWith(target, "maps\\_zombiemode_perks"))
            {
                const auto parts = wawPerkParts.find(target);
                if (parts == wawPerkParts.end() || !parts->second.count(low))
                    continue;
            }
            const auto& dogs = A["zombie_runtime"]["dog_rounds"];
            const bool dogsDriven = dogs.contains("source_driven") && Truthy(dogs["source_driven"]);
            if (target == Norm(dogs["source_script"].get<std::string>()) && low == dogs["source_init"].get<std::string>() && !dogsDriven)
            {
                const auto call = DogRoundsHook();
                if (call && !call->empty() && std::find(calls.begin(), calls.end(), *call) == calls.end())
                    calls.push_back(*call);
                continue;
            }
            if (target == Norm(dogs["source_script"].get<std::string>()) && low == dogs["source_init"].get<std::string>())
            {
                for (const auto& name : ClientFogMonitors(dogs["client_notifies"]))
                    calls.push_back("level thread " + std::string(CORE) + "::" + name + "();");
                report.rewrites["WaW drives the dog rounds: source _zombiemode_dogs"] += 1;
            }
            const auto mapped = ApiString(A["core_scripts"], target);
            const auto argc = Argc(zm->tokens, static_cast<long long>(ref.index));
            if (ApiGet(A.value("signature_source_scripts", py::Json::object()), target) && mapped && !mapped->empty() && !api.Defines(*mapped, low)
                && !(argc && *argc))
            {
                const auto extracted = Extract(target, low);
                if (extracted && !extracted->empty() && std::find(calls.begin(), calls.end(), *extracted + "();") == calls.end())
                {
                    calls.push_back(*extracted + "();");
                    report.rewrites["framework hook " + target + "::" + ref.name + " -> source (BO2 has none)"] += 1;
                }
                continue;
            }
            if (IsCore(target, sources))
                continue;
            const auto dep = sources.Get(target);
            const Function* hook = dep ? dep->Find(low) : nullptr;
            std::string prefix;
            long long previous = static_cast<long long>(*ref.qualIndex) - 1;
            if (At(zm->tokens, previous).Low() == "thread")
            {
                prefix = "thread ";
                previous--;
            }
            if (In(At(zm->tokens, previous).Low(), {"level", "self"}))
                prefix = At(zm->tokens, previous).text + " " + prefix;
            const auto call = prefix + PortedPath(target) + "::" + ref.name + "();";
            if (!hook || std::find(calls.begin(), calls.end(), call) != calls.end())
                continue;
            if (!hook->params.empty() || (argc && *argc))
            {
                report.Unsupported(FALLBACK, whereFile + ":" + std::to_string(zm->tokens[ref.index].line) + ": framework hook " + target + "::" + ref.name + " takes arguments; not called");
                continue;
            }
            Want(target);
            calls.push_back(call);
            report.rewrites["framework hook " + target + "::" + ref.name + " -> waw_main_post"] += 1;
        }
        return calls;
    }

    void Translator::SplitMain(Script& script)
    {
        const Function* fnp = script.Find("main");
        if (!fnp)
        {
            report.errors.push_back(script.path + ": map main has no main()");
            return;
        }
        const Function fn = *fnp;
        auto& tokens = script.tokens;
        std::string hooks;
        for (const auto& call : FrameworkHooks())
            hooks += "\n\t" + call;
        if (!hooks.empty())
            hooks = "\n\t// WaW _zombiemode::main inits of the map's own scripts" + hooks;
        if (!postCalls.empty())
        {
            hooks += "\n\t// the map's _zombiemode overrides BO2's _zm::init replaced";
            for (const auto& call : postCalls)
                hooks += "\n\t" + call;
        }
        tokens[fn.start].text = "waw_main_pre";
        const auto weapons = sources.Get(WEAPONS_FRAMEWORK);
        if (weapons && weapons->Find("include_zombie_weapon"))
        {
            Want(WEAPONS_FRAMEWORK);
            preCalls.insert(preCalls.begin(), "level.waw_include_zombie_weapon = " + PortedPath(WEAPONS_FRAMEWORK) + "::include_zombie_weapon;");
        }
        const auto score = sources.Get(SCORE_FRAMEWORK);
        if (score && score->Find("player_add_points"))
        {
            Want(SCORE_FRAMEWORK);
            preCalls.insert(preCalls.begin(), "level.waw_player_add_points = " + PortedPath(SCORE_FRAMEWORK) + "::player_add_points;");
        }
        if (const auto music = ExtractEndGameMusic())
            preCalls.insert(preCalls.begin(), "level.waw2bo2_end_game_music = " + std::string(CORE) + "::" + *music + ";");
        if (const auto connect = ExtractPlayerConnectFields())
            preCalls.insert(preCalls.begin(), "level thread " + std::string(CORE) + "::" + *connect + "();");
        std::string pre;
        for (const auto& call : preCalls)
            pre += "\n\t" + call;
        if (!pre.empty())
            pre = "\n\t// the map's functions in place of BO2's, before _zm::init calls them" + pre;
        tokens[fn.bodyOpen].text = "{\n\t" + std::string(COMPAT) + "::init();\n\t" + CORE + "::framework_level_state();" + pre + "\n\t" + PRECACHE + "::init();\n\t" + ONEWAY
                                   + "::init();\n\t" + WATER_BULLETS + "::init();";
        std::optional<Ref> split;
        for (const auto& ref : References(tokens, fn.bodyOpen, fn.bodyClose))
            if (ref.qualifier && Norm(*ref.qualifier) == "maps\\_zombiemode" && Lower(ref.name) == "main")
            {
                split = ref;
                break;
            }
        if (!split)
        {
            tokens[fn.bodyClose].text = "}\n\nwaw_main_post()\n{" + hooks + "\n}";
            report.Unsupported(FALLBACK, script.path + ": no maps\\_zombiemode::main() call; whole main runs before _zm::init");
            return;
        }
        const auto i = static_cast<long long>(*split->qualIndex);
        long long end = i;
        while (At(tokens, end).text != ";")
            end++;
        if (const auto lists = ExtractSpawnerLists())
        {
            const auto close = static_cast<long long>(Match(tokens, split->index + 1, "(", ")"));
            const auto args = PyStrip(EmitRange(tokens, static_cast<long long>(split->index) + 2, close));
            hooks = "\n\t" + std::string(CORE) + "::" + *lists + "( " + args + " );" + hooks;
        }
        At(tokens, i).text = "}\n\nwaw_main_post()\n{" + hooks
                             + "\n\tcommon_scripts\\utility::flag_wait( \"initial_players_connected\" );\t// WaW _zombiemode::main waits for all players\n";
        for (long long k = i + 1; k < end + 1; k++)
        {
            At(tokens, k).text = "";
            At(tokens, k).kind = Kind::Punct;
        }
        At(tokens, i).kind = Kind::Punct;
    }

    std::string Translator::StubSource() const
    {
        std::vector<std::string> lines{"// waw2bo2: WaW functions/builtins with no BO2 translation yet. Each reports itself once.", "// Listed in the stage report under 'unsupported'.",
                                       ""};
        for (const auto& [stub, entry] : stubs)
        {
            std::vector<std::string> params;
            for (int i = 0; i < entry.second; i++)
                params.push_back("p" + std::to_string(i));
            lines.push_back(stub + "(" + Join(params, ", ") + ")");
            lines.push_back("{");
            lines.push_back(std::string("    ") + COMPAT + "::report( \"GSC\", \"" + Replace(entry.first, "\\", "\\\\") + "\" );");
            lines.push_back("}");
            lines.push_back("");
        }
        return Join(lines, "\n");
    }
} // namespace w2b::gsc::port

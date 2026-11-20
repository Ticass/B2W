// Zombie appearance, stock perks, hint defaults, the perk table and the
// one-way clip and water bullet script sources (see support.h).
#include "support.h"

#include "core/encoding.h"
#include "core/files.h"
#include "core/regex.h"
#include "gsc/parse.h"
#include "mapents/map_entity.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace w2b::gsc::port
{
    namespace fs = std::filesystem;

    namespace
    {
        util::Json LoadJson(const fs::path& path)
        {
            return util::Json::parse(files::ReadText(path));
        }

        bool Truthy(const util::Json* value)
        {
            if (!value || value->is_null())
                return false;
            if (value->is_string())
                return !value->get_ref<const std::string&>().empty();
            if (value->is_boolean())
                return value->get<bool>();
            if (value->is_number())
                return value->get<double>() != 0.0;
            return !value->empty();
        }

        const util::Json* Get(const util::Json& object, const std::string& key)
        {
            if (!object.is_object())
                return nullptr;
            const auto it = object.find(key);
            return it == object.end() ? nullptr : &*it;
        }

        std::string GetString(const util::Json& object, const std::string& key)
        {
            const auto* v = Get(object, key);
            return v && v->is_string() ? v->get<std::string>() : std::string();
        }

        std::string LowerAscii(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        // Path.glob(prefix*suffix) in one folder (case-insensitive, as on Windows)
        std::vector<fs::path> GlobFolder(const fs::path& folder, const std::string& prefix, const std::string& suffix)
        {
            std::vector<fs::path> found;
            std::error_code error;
            if (!fs::is_directory(folder, error))
                return found;
            for (const auto& entry : fs::directory_iterator(folder))
            {
                const auto name = LowerAscii(files::Utf8(entry.path().filename()));
                if (name.size() >= prefix.size() + suffix.size() && StartsWith(name, prefix) && EndsWith(name, suffix))
                    found.push_back(entry.path());
            }
            return found;
        }
    } // namespace

    std::string FormatG(const double value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%g", value);
        return buffer;
    }

    // ---------------------------------------------------------------- zombieappearance
    namespace zombieappearance
    {
        std::string ActorName(const std::string& source)
        {
            return "waw_" + source;
        }

        std::string AppearanceScript(const std::string& source)
        {
            auto script = Parse(source, "<WaW actor>");
            auto& tokens = script.tokens;
            static const std::set<std::string> engineFields{"animtree", "animstatedef", "team", "type", "accuracy", "health",
                                                            "weapon", "secondaryweapon", "sidearm", "grenadeweapon", "grenadeammo", "subclass"};
            for (const char* name : {"main", "precache"})
            {
                const Function* fn = script.Find(name);
                if (!fn)
                    throw std::runtime_error(std::string("WaW actor has no ") + name + "()");
                size_t start = fn->bodyOpen + 1;
                int depth = 0;
                for (size_t i = fn->bodyOpen + 1; i < fn->bodyClose; i++)
                {
                    depth += In(tokens[i].text, {"(", "[", "{"});
                    depth -= In(tokens[i].text, {")", "]", "}"});
                    if (tokens[i].text != ";" || depth)
                        continue;
                    std::vector<std::string> words;
                    for (size_t k = start; k < i + 1; k++)
                        words.push_back(tokens[k].Low());
                    const bool engineAssignment = words.size() > 3 && words[0] == "self" && words[1] == "." && words[3] == "=" && engineFields.count(words[2]);
                    const bool engineCall = words.size() > 1 && words[0] == "self" && In(words[1], {"setengagementmindist", "setengagementmaxdist"});
                    const bool itemPrecache = !words.empty() && words[0] == "precacheitem";
                    if (engineAssignment || engineCall || itemPrecache)
                        for (size_t k = start; k < i + 1; k++)
                            tokens[k].text = tokens[k].pre = "";
                    start = i + 1;
                }
            }
            return Emit(tokens);
        }

        std::string NativeActor(const std::string& tmpl, const std::string& sourcePath)
        {
            auto script = Parse(tmpl, "<T6 actor>");
            auto& tokens = script.tokens;
            const Function main = *script.Find("main");
            std::optional<Ref> random;
            for (const auto& r : References(tokens, main.bodyOpen, main.bodyClose))
                if (Lower(r.name) == "get_random_character")
                {
                    random = r;
                    break;
                }
            if (!random || !random->qualIndex)
                throw std::runtime_error("T6 actor template: no get_random_character call");
            size_t start = *random->qualIndex;
            while (start > main.bodyOpen && !In(tokens[start - 1].text, {";", "{"}))
                start--;
            tokens[start].text = sourcePath + "::main();\n    self setcharacterindex( 0 );";
            for (size_t k = start + 1; k < main.bodyClose; k++)
                tokens[k].text = tokens[k].pre = "";
            static const regex::Regex characterInclude(R"(#include\s+character\\)", regex::IGNORECASE);
            for (auto& token : tokens)
                if (token.kind == Kind::Directive && characterInclude.MatchAt(token.text))
                    token.text = "";
            const Function fn = *script.Find("precache");
            for (const auto& ref : References(tokens, fn.bodyOpen, fn.bodyClose))
                if (ref.qualifier && StartsWith(Lower(*ref.qualifier), "character\\"))
                {
                    size_t end = ref.index;
                    while (tokens.at(end).text != ";")
                        end++;
                    for (size_t k = *ref.qualIndex; k < end + 1; k++)
                        tokens[k].text = tokens[k].pre = "";
                }
            tokens[fn.bodyClose].text = "    " + sourcePath + "::precache();\n}";
            return "// waw2bo2: source WaW zombie appearance, T6 actor state support.\n" + Emit(tokens);
        }

        std::string SourceActorKind(const std::string& source)
        {
            const auto script = Parse(source, "<WaW actor>");
            if (const Function* fn = script.Find("main"))
            {
                const auto& toks = script.tokens;
                for (long long i = static_cast<long long>(fn->bodyOpen) + 1; i < static_cast<long long>(fn->bodyClose) - 4; i++)
                    if (toks[i].Low() == "self" && toks[i + 1].text == "." && toks[i + 2].Low() == "type" && toks[i + 3].text == "="
                        && toks[i + 4].kind == Kind::String)
                        return Lower(StripChars(toks[i + 4].text, "\""));
            }
            return "human";
        }

        std::string TemplateTree(const std::string& tmpl)
        {
            static const regex::Regex tree(R"re(#using_animtree\(\s*"([^"]+)"\s*\))re");
            const auto match = tree.Search(tmpl);
            if (!match)
                throw std::runtime_error("T6 actor template has no #using_animtree");
            return (*match)[1];
        }

        std::string NativeClientActor(const std::string& name, const std::string& tree)
        {
            return "// waw2bo2: replicated WaW appearance, T6 client AI state support.\n#using_animtree(\"" + tree + "\");\nmain()\n{\n    self._aitype = \""
                   + ActorName(name) + "\";\n}\nprecache( ai_index )\n{\n    usefootsteptable( ai_index, \"default_ai\" );\n"
                   + "    precacheanimstatedef( ai_index, #animtree, \"" + tree + "\" );\n"
                   + "    setdemolockonvalues( ai_index, 100, 60, -15, 60, 30, -5, 60 );\n}\n";
        }

        std::set<std::string> SourceActorTypes(const fs::path& projectRoot)
        {
            const auto path = projectRoot / "BSP/entities.json";
            std::set<std::string> names;
            std::error_code error;
            if (!fs::exists(path, error))
                return names;
            for (const auto& ent : mapents::ReadMapEntities(path))
            {
                if (const auto* type = ent.Find("waw_actor_type"))
                    names.insert(*type);
                else if (const auto classname = std::string(ent.Get("classname")); StartsWith(classname, "actor_waw_"))
                    names.insert(classname.substr(10));
            }
            return names;
        }

        void BindActorTypes(const fs::path& projectRoot, const std::set<std::string>& names)
        {
            const auto path = projectRoot / "BSP/entities.json";
            auto entities = mapents::ReadMapEntities(path);
            for (auto& ent : entities)
            {
                const auto* found = ent.Find("waw_actor_type");
                if (!found)
                    continue;
                const auto value = *found;
                ent.Erase("waw_actor_type");
                if (names.count(value))
                    ent.Set("classname", "actor_" + ActorName(value));
            }
            mapents::WriteMapEntities(path, entities);
        }
    } // namespace zombieappearance

    // ---------------------------------------------------------------- stockperks
    namespace stockperks
    {
        namespace
        {
            using Pairs = std::vector<std::pair<std::string, std::string>>;
            const Pairs PERK_NAMES{{"specialty_detectexplosive", "specialty_flakjacket"}, {"specialty_extraammo", "specialty_additionalprimaryweapon"},
                                   {"specialty_bulletaccuracy", "specialty_deadshot"},   {"specialty_boost", "specialty_grenadepulldeath"},
                                   {"specialty_quieter", "specialty_scavenger"},         {"specialty_shades", "specialty_finalstand"}};
            const Pairs GROUPS{{"zombie_vending_bo1", "zombie_vending"}, {"zombie_vending_bo2", "zombie_vending"}};
            const Pairs MACHINE_NAMES{{"vending_mule_kick", "vending_additionalprimaryweapon"},
                                      {"vending_staminup", "vending_marathon"},
                                      {"vending_phd_flopper", "vending_divetonuke"},
                                      {"vending_whos_who", "vending_chugabud"},
                                      {"mule_kick_on", "additionalprimaryweapon_on"},
                                      {"staminup_on", "marathon_on"},
                                      {"phd_on", "divetonuke_on"},
                                      {"cherry_on", "electric_cherry_on"}};
            const char* const FLAGS[] = {"doubletap", "juggernaut", "revive", "sleightofhand", "marathon", "deadshot", "additionalprimaryweapon", "tombstone", "chugabud"};
            const char* const MODULES[] = {"electric_cherry", "divetonuke"};
            const Pairs STOCK_MODELS{{"zombie_vending_ads", "p6_zm_al_vending_ads_on"},
                                     {"zombie_vending_ads_on", "p6_zm_al_vending_ads_on"},
                                     {"zombie_vending_nuke", "p6_zm_al_vending_nuke_on"},
                                     {"zombie_vending_nuke_on", "p6_zm_al_vending_nuke_on"}};
            const Pairs OFF_MODELS{{"specialty_armorvest", "zombie_vending_jugg"},
                                   {"specialty_quickrevive", "zombie_vending_revive"},
                                   {"specialty_fastreload", "zombie_vending_sleight"},
                                   {"specialty_rof", "zombie_vending_doubletap2"},
                                   {"specialty_longersprint", "zombie_vending_marathon"},
                                   {"specialty_deadshot", "zombie_vending_ads"},
                                   {"specialty_additionalprimaryweapon", "zombie_vending_three_gun"},
                                   {"specialty_flakjacket", "zombie_vending_nuke"},
                                   {"specialty_grenadepulldeath", "p6_zm_vending_electric_cherry_off"}};
            const char* const NATIVE_PACK_A_PUNCH[] = {
                "    if ( vending_weapon_upgrade_trigger.size >= 1 )\n        array_thread( vending_weapon_upgrade_trigger, ::vending_weapon_upgrade );\n",
                "    if ( isdefined( level._custom_turn_packapunch_on ) )\n        level thread [[ level._custom_turn_packapunch_on ]]();\n    else\n        level thread turn_packapunch_on();\n"};

            const std::string* Lookup(const Pairs& pairs, const std::string& key)
            {
                for (const auto& [k, v] : pairs)
                    if (k == key)
                        return &v;
                return nullptr;
            }

            bool Supported(const std::string& perk)
            {
                for (const char* p : {"specialty_armorvest", "specialty_quickrevive", "specialty_rof", "specialty_fastreload", "specialty_longersprint", "specialty_weapupgrade"})
                    if (perk == p)
                        return true;
                for (const auto& [_, v] : PERK_NAMES)
                    if (perk == v)
                        return true;
                return false;
            }

            bool HasRawModel(const fs::path& bo2Root, const std::string& name)
            {
                std::error_code error;
                return fs::is_regular_file(bo2Root / files::FromUtf8("raw/xmodel/" + name + ".json"), error);
            }

            std::string ReplaceMissingModels(std::string source, const fs::path& bo2Root)
            {
                for (const auto& [old, replacement] : STOCK_MODELS)
                    if (!HasRawModel(bo2Root, old))
                        source = Replace(source, "\"" + old + "\"", "\"" + replacement + "\"");
                return source;
            }
        } // namespace

        bool IsPerkFramework(const std::string& path)
        {
            return StartsWith(path, "maps\\_zombiemode_perks");
        }

        std::string TranslateValue(const std::string& value)
        {
            if (const auto* g = Lookup(GROUPS, value))
                return *g;
            if (const auto* m = Lookup(MACHINE_NAMES, value))
                return *m;
            for (const auto& [old, replacement] : PERK_NAMES)
                if (value == old || StartsWith(value, old + "_"))
                    return replacement + value.substr(old.size());
            return value;
        }

        std::string TranslateLiterals(const std::string& source)
        {
            auto tokens = Tokenize(source);
            for (auto& token : tokens)
                if (token.kind == Kind::String && StartsWith(token.text, "\""))
                    token.text = "\"" + TranslateValue(token.text.size() >= 2 ? token.text.substr(1, token.text.size() - 2) : std::string()) + "\"";
            return Emit(tokens);
        }

        std::string StockModel(const std::string& name, const fs::path& bo2Root)
        {
            if (const auto* model = Lookup(STOCK_MODELS, name); model && !HasRawModel(bo2Root, name))
                return *model;
            return name;
        }

        std::set<std::string> MachineModels(const fs::path& bo2Root)
        {
            std::set<std::string> names;
            static const regex::Regex precache(R"re(precachemodel\(\s*"([^"\n]+)")re", regex::IGNORECASE);
            std::vector<std::string> modules{"perks"};
            for (const char* m : MODULES)
                modules.push_back(std::string("perk_") + m);
            for (const auto& module : modules)
            {
                const auto text = files::ReadText(bo2Root / files::FromUtf8("raw/maps/mp/zombies/_zm_" + module + ".gsc"));
                for (const auto& row : precache.FindAll(text))
                    if (Contains(row[0], "vending"))
                        names.insert(StockModel(row[0], bo2Root));
            }
            return names;
        }

        void TranslateEntities(const fs::path& path, const fs::path* bo2Root)
        {
            auto entities = mapents::ReadMapEntities(path);
            std::map<std::string, std::string> machines;
            for (auto& entity : entities)
            {
                for (auto& [key, value] : entity.fields)
                    value = TranslateValue(value);
                const auto targetname = std::string(entity.Get("targetname"));
                if (!entity.Has("targetname") || !In(targetname, {"zombie_vending", "zombie_vending_upgrade"}))
                    continue;
                const auto* perkValue = entity.Find("script_noteworthy");
                const auto perk = perkValue ? *perkValue : std::string();
                if (!perk.empty())
                {
                    const auto base = EndsWith(perk, "_upgrade") ? perk.substr(0, perk.size() - 8) : perk;
                    if (!Supported(base))
                        throw std::runtime_error("BO2 stock perks: vending machine uses perk '" + perk
                                                 + "', which BO2 does not have (unsupported perk; turn \"Use BO2 stock perks\" off for this map)");
                }
                const auto* off = Lookup(OFF_MODELS, perk);
                if (targetname == "zombie_vending" && perkValue && off && !entity.Get("target").empty())
                    machines[std::string(entity.Get("target"))] = *off;
            }
            if (bo2Root)
                for (auto& entity : entities)
                {
                    if (!entity.Has("targetname"))
                        continue;
                    const auto model = machines.find(std::string(entity.Get("targetname")));
                    if (model != machines.end() && !model->second.empty() && entity.Get("classname") == "script_model")
                        entity.Set("model", StockModel(model->second, *bo2Root));
                }
            mapents::WriteMapEntities(path, entities);
        }

        std::string NativeSource(std::string source, const std::string& name, const fs::path& outRoot, const fs::path& bo2Root)
        {
            const bool client = StartsWith(name, "clientscripts/");
            source = ReplaceMissingModels(source, bo2Root);
            if (!client)
                for (const char* block : NATIVE_PACK_A_PUNCH)
                {
                    if (!Contains(source, block))
                        throw std::runtime_error(name + ": BO2 Pack-a-Punch start not found");
                    source = Replace(source, block, "    // waw2bo2: the source map's WaW Pack-a-Punch stays in charge.\n");
                }
            const std::string prefix = client ? "clientscripts" : "maps";
            const auto native = prefix + "\\mp\\zombies\\_zm_perks";
            const auto owned = prefix + "\\mp\\waw\\_waw2bo2_perks";
            std::vector<std::string> calls;
            for (const char* flag : FLAGS)
                calls.push_back(std::string("    level.zombiemode_using_") + flag + "_perk = 1;");
            if (!client)
                calls.push_back("    level.waw2bo2_stock_perks = 1;");
            const std::string ext = client ? ".csc" : ".gsc";
            for (const char* module : MODULES)
            {
                const auto relative = prefix + "/mp/zombies/_zm_perk_" + module + ext;
                const auto raw = bo2Root / "raw" / files::FromUtf8(relative);
                std::error_code error;
                if (!fs::is_regular_file(raw, error))
                    throw std::runtime_error("BO2 stock perk runtime missing: " + files::Utf8(raw));
                const auto target = outRoot / files::FromUtf8(prefix + "/mp/waw/_waw2bo2_perk_" + module + ext);
                auto moduleSource = ReplaceMissingModels(Replace(files::ReadText(raw), native, owned), bo2Root);
                if (std::string(module) == "electric_cherry")
                {
                    moduleSource = Replace(moduleSource, "\"vendingelectric_cherry\"", "\"vending_electric_cherry\"");
                    moduleSource = Replace(moduleSource, "\"vending_electriccherry\"", "\"vending_electric_cherry\"");
                }
                files::WriteText(target, moduleSource);
                calls.push_back("    " + prefix + "\\mp\\waw\\_waw2bo2_perk_" + module + "::enable_" + module + "_perk_for_level();");
            }
            auto script = Parse(source, name);
            const Function* init = script.Find("init");
            if (!init)
                throw std::runtime_error(name + ": BO2 perk initializer missing");
            script.tokens[init->bodyOpen].text += "\n" + Join(calls, "\n") + "\n";
            return Emit(script.tokens);
        }

        std::vector<std::string> AssetLines(const fs::path& outRoot)
        {
            std::set<std::string> entries;
            std::vector<fs::path> paths{outRoot / "maps/mp/waw/_waw2bo2_perks.gsc"};
            for (const auto& p : GlobFolder(outRoot / "maps/mp/waw", "_waw2bo2_perk_", ".gsc"))
                paths.push_back(p);
            for (const auto& p : GlobFolder(outRoot / "clientscripts/mp/waw", "_waw2bo2_perk_", ".csc"))
                paths.push_back(p);
            static const regex::Regex call(R"re(\b(precacheitem|precachemodel|precacheshader|loadfx)\s*\(\s*"([^"\n]+)")re", regex::IGNORECASE);
            for (const auto& path : paths)
                for (const auto& row : call.FindAll(files::ReadText(path)))
                {
                    const auto kind = Lower(row[0]);
                    const char* asset = kind == "precacheitem" ? "weapon" : kind == "precachemodel" ? "xmodel" : kind == "precacheshader" ? "material" : "fx";
                    entries.insert(std::string(asset) + "," + row[1]);
                }
            return std::vector<std::string>(entries.begin(), entries.end());
        }
    } // namespace stockperks

    // ---------------------------------------------------------------- hintdefaults
    namespace hintdefaults
    {
        std::string Source(const fs::path& project)
        {
            const auto path = project / "BSP/entities.json";
            std::error_code error;
            mapents::MapEntities entities;
            if (fs::exists(path, error))
                entities = mapents::ReadMapEntities(path);
            std::map<std::string, std::vector<const mapents::MapEntity*>> groups;
            for (const auto& entity : entities)
                if (entity.Get("classname") == "trigger_use" && !entity.Get("targetname").empty())
                    groups[std::string(entity.Get("targetname"))].push_back(&entity);
            std::vector<std::string> names;
            for (const auto& [name, group] : groups)
            {
                bool bare = true;
                for (const auto* entity : group)
                    bare = bare && entity->Get("hintstring").empty() && entity->Get("script_hint").empty();
                if (bare)
                    names.push_back(name);
            }
            std::vector<std::string> lines{"// WaW default use prompts; source scripts may replace or clear them.", "init()", "{"};
            if (!names.empty())
                lines.push_back("    precachestring(&\"PLATFORM_HOLD_TO_USE\");");
            for (const auto& name : names)
            {
                lines.push_back("    triggers = getentarray(" + util::JsonDumps(util::Json(name), 2) + ", \"targetname\");");
                lines.push_back("    for (i = 0; i < triggers.size; i++)");
                lines.push_back("        if (triggers[i].classname == \"trigger_use\")");
                lines.push_back("            triggers[i] sethintstring(&\"PLATFORM_HOLD_TO_USE\");");
            }
            lines.push_back("}");
            lines.push_back("");
            return Join(lines, "\n");
        }
    } // namespace hintdefaults

    // ---------------------------------------------------------------- perktable
    namespace perktable
    {
        std::optional<fs::path> T6Executable()
        {
            const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
            if (!local)
                throw std::runtime_error("KeyError: 'LOCALAPPDATA'");
            const auto executable = fs::path(local) / "Plutonium" / "games" / "t6zm.exe";
            std::error_code error;
            if (fs::is_regular_file(executable, error))
                return executable;
            return std::nullopt;
        }

        std::set<std::string> EnginePerks(const fs::path& executable)
        {
            std::string data;
            if (!files::ReadBytes(executable, data))
                throw std::runtime_error("cannot read " + files::Utf8(executable));
            const auto word = [](const char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
            const auto lowerWord = [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; };
            std::set<std::string> perks;
            const std::string_view key = "specialty_";
            for (size_t at = data.find(key); at != std::string::npos; at = data.find(key, at + 1))
            {
                if (at > 0 && word(data[at - 1]))
                    continue;
                size_t end = at + key.size();
                while (end < data.size() && lowerWord(data[end]))
                    end++;
                if (end == at + key.size() || end >= data.size() || data[end] != '\0')
                    continue;
                perks.insert(data.substr(at, end - at));
                at = end - 1;
            }
            return perks;
        }

        std::set<std::string> MapPerks(const fs::path& projectRoot)
        {
            std::vector<std::string> texts;
            for (const auto& p : files::GlobSuffix(projectRoot / "maps/mp/waw", ".gsc"))
            {
                std::string bytes;
                files::ReadBytes(p, bytes);
                texts.push_back(encoding::UniversalNewlines(encoding::Utf8Replace(bytes)));
            }
            const auto entities = projectRoot / "BSP/entities.json";
            std::error_code error;
            if (fs::is_regular_file(entities, error))
            {
                std::string bytes;
                files::ReadBytes(entities, bytes);
                texts.push_back(encoding::UniversalNewlines(encoding::Utf8Replace(bytes)));
            }
            static const regex::Regex literal(R"re("(specialty_[A-Za-z0-9_]+)")re");
            std::set<std::string> perks;
            for (const auto& text : texts)
                for (const auto& row : literal.FindAll(text))
                    perks.insert(Lower(row[0]));
            return perks;
        }

        std::string Source(const std::set<std::string>& emulated)
        {
            std::string lines;
            for (const auto& perk : emulated)
                lines += "    level.waw2bo2_emulated_perks[\"" + perk + "\"] = 1;\n";
            return "// waw2bo2: the map's perk names T6's perk table lacks\ninit()\n{\n    level.waw2bo2_emulated_perks = [];\n" + lines + "}\n";
        }

        util::Json Stage(const fs::path& projectRoot, const std::optional<fs::path>& executable)
        {
            const auto scriptDir = projectRoot / "maps/mp/waw";
            const auto exe = executable ? executable : T6Executable();
            util::Json result = util::Json::object();
            if (!exe)
            {
                files::WriteText(scriptDir / SCRIPT, Source({}));
                result["error"] = "T6 executable (Plutonium games/t6zm.exe) not found: perk names T6 lacks are not detected";
                return result;
            }
            const auto known = EnginePerks(*exe);
            if (known.empty())
                throw std::runtime_error(files::Utf8(*exe) + ": no perk table found");
            const auto used = MapPerks(projectRoot);
            std::set<std::string> emulated;
            std::set_difference(used.begin(), used.end(), known.begin(), known.end(), std::inserter(emulated, emulated.begin()));
            files::WriteText(scriptDir / SCRIPT, Source(emulated));
            result["engine_perks"] = known.size();
            result["map_perks"] = used.size();
            result["emulated"] = std::vector<std::string>(emulated.begin(), emulated.end());
            return result;
        }
    } // namespace perktable

    // ---------------------------------------------------------------- oneway / waterbullets
    namespace oneway
    {
        std::string OnewaySource()
        {
            return "// waw2bo2: one-sided traversal collision is omitted at export.\n\ninit()\n{\n}\n";
        }
    } // namespace oneway

    namespace waterbullets
    {
        namespace
        {
            constexpr std::string_view WATER_BULLETS_GSC = R"GSC(// waw2bo2: WaW bullets go on through water; T6 never damages what stands in it.
// Each row: hip stand/ducked/prone min, hip
// stand/ducked/prone max, decay rate, fire add, move add, ADS spread, pellets,
// WaW water penetration depth.

init()
{
    level.waw_water_bullets = [];
    // WEAPON_TABLE
    level thread waw_water_bullet_players();
}

waw_water_row( a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 )
{
    row = [];
    row[0] = a0;
    row[1] = a1;
    row[2] = a2;
    row[3] = a3;
    row[4] = a4;
    row[5] = a5;
    row[6] = a6;
    row[7] = a7;
    row[8] = a8;
    row[9] = a9;
    row[10] = a10;
    row[11] = a11;
    return row;
}

waw_water_bullet_players()
{
    for (;;)
    {
        level waittill( "connected", player );
        player thread waw_water_bullets();
    }
}

waw_water_bullets()
{
    self endon( "disconnect" );
    self.waw_water_spread = 0;
    self.waw_water_spread_time = gettime();

    for (;;)
    {
        self waittill( "weapon_fired", weapon );

        if ( !isdefined( weapon ) || !isdefined( level.waw_water_bullets[weapon] ) )
            continue;

        row = level.waw_water_bullets[weapon];
        spread = self waw_water_spread( row );
        eye = self geteye();
        angles = self getplayerangles();
        forward = anglestoforward( angles );
        right = anglestoright( angles );
        up = anglestoup( angles );
        cone = tan( spread );

        for ( i = 0; i < row[10]; i++ )
        {
            radius = randomfloat( 1 ) * cone;
            turn = randomfloat( 360 );
            dir = vectornormalize( forward + right * ( cos( turn ) * radius ) + up * ( sin( turn ) * radius ) );
            trace = bullettrace( eye, eye + dir * 8192, 1, self );

            if ( !isdefined( trace["surfacetype"] ) || trace["surfacetype"] != "water" || isdefined( trace["entity"] ) )
                continue;

            start = trace["position"] + dir * 0.5;
            magicbullet( weapon, start, start + dir * row[11], self );
        }
    }
}

// Hip spread as both engines evolve it: stance minimum, plus the fire add
// decaying at its rate, plus the move add; blended to ADS spread.
waw_water_spread( row )
{
    stance = self getstance();
    low = row[0];
    high = row[3];

    if ( stance == "crouch" )
    {
        low = row[1];
        high = row[4];
    }
    else if ( stance == "prone" )
    {
        low = row[2];
        high = row[5];
    }

    now = gettime();
    self.waw_water_spread = max( 0, self.waw_water_spread - row[6] * ( now - self.waw_water_spread_time ) / 1000 );
    self.waw_water_spread_time = now;
    velocity = self getvelocity();
    moving = min( 1, length( ( velocity[0], velocity[1], 0 ) ) / max( 1, getdvarfloat( "g_speed" ) ) );
    hip = min( high, low + self.waw_water_spread + row[8] * moving );
    self.waw_water_spread = min( high - low, self.waw_water_spread + row[7] );
    ads = self playerads();
    return hip + ( row[9] - hip ) * ads;
}
)GSC";
        } // namespace

        std::map<std::string, double> WaterDepths(const std::string& tableText)
        {
            const auto values = util::Split(util::Strip(tableText), "\\");
            std::map<std::string, std::string> pairs;
            for (size_t i = 1; i + 1 < values.size(); i += 2)
                pairs[std::string(values[i])] = std::string(values[i + 1]);
            std::map<std::string, double> depths;
            for (const char* kind : {"small", "medium", "large"})
                if (const auto it = pairs.find(std::string(kind) + "_water"); it != pairs.end())
                {
                    double value = 0;
                    if (!util::ParseFloat(it->second, value))
                        throw std::invalid_argument("could not convert string to float: '" + it->second + "'");
                    depths[kind] = value;
                }
            return depths;
        }

        std::string WaterBulletsSource(const std::map<std::string, std::vector<double>>& rows)
        {
            if (rows.empty())
                return "// waw2bo2: no water brushes or no bullet weapon penetrates water.\n\ninit()\n{\n}\n";
            std::string table;
            for (const auto& [name, row] : rows)
            {
                std::vector<std::string> values;
                for (const double v : row)
                    values.push_back(FormatG(v));
                table += "    level.waw_water_bullets[\"" + name + "\"] = waw_water_row( " + Join(values, ", ") + " );\n";
            }
            return Replace(std::string(WATER_BULLETS_GSC), "    // WEAPON_TABLE\n", table);
        }
    } // namespace waterbullets
} // namespace w2b::gsc::port

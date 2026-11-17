// Port of Translator.translate / adapt_native_arguments / resolve_refs and the
// animation and lowering helpers (gscport.py 1615-1942, 2554-2634).
#include "translator.h"

#include "api.h"
#include "core/pyre.h"
#include "rewrites.h"

namespace w2b::gsc::port
{
    namespace
    {
        // Run a rewrite on tokens[a:b] as Python runs it on a slice: positions
        // count within the slice, attribute changes reach the originals.
        template<typename Fn> void OnSlice(Tokens& tokens, const long long a, const long long b, Fn&& fn)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            Tokens part(tokens.begin() + static_cast<long long>(lo), tokens.begin() + static_cast<long long>(hi));
            fn(part);
            for (size_t k = 0; k < part.size(); k++)
                tokens[lo + k] = part[k];
        }

        Tokens SliceCopy(const Tokens& tokens, const long long a, const long long b)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            return Tokens(tokens.begin() + static_cast<long long>(lo), tokens.begin() + static_cast<long long>(hi));
        }

        bool IsPerkFramework(const std::optional<std::string>& path)
        {
            return path && StartsWith(*path, "maps\\_zombiemode_perks");
        }

        const pyre::Regex& IncludeRe()
        {
            static const pyre::Regex re(R"(#include\s+([\w\\/]+)\s*;?)", pyre::IGNORECASE);
            return re;
        }

        const pyre::Regex& AnimtreeRe()
        {
            static const pyre::Regex re(R"re(#using_animtree\s*\(\s*"([^"]+)"\s*\))re", pyre::IGNORECASE);
            return re;
        }
    } // namespace

    std::string Translator::Translate(const std::string& path, const bool mainSplit)
    {
        const auto script = sources.Get(path);
        auto& tokens = script->tokens;
        const auto& A = Api();
        if (const auto* glues = ApiGet(A["zombie_runtime"], "actor_glue"))
            for (const auto& [key, glue] : glues->items())
            {
                const auto cut = key.rfind("::");
                const auto owner = cut == std::string::npos ? std::string() : key.substr(0, cut);
                const auto name = cut == std::string::npos ? key : key.substr(cut + 2);
                const Function* fn = Norm(owner) == Norm(path) ? script->Find(Lower(name)) : nullptr;
                if (fn && tokens[fn->bodyOpen].text == "{")
                {
                    tokens[fn->bodyOpen].text = "{\n\tself " + glue.get<std::string>() + "();";
                    report.rewrites["T6 actor glue " + glue.get<std::string>() + " -> " + key] += 1;
                }
            }
        const auto* drivenTable = ApiGet(A.value("source_driven", py::Json::object()), Norm(path));
        const auto* glue = drivenTable ? ApiGet(*drivenTable, "glue") : nullptr;
        const auto counterpart = ApiString(A["core_scripts"], Norm(path));
        if (glue && !glue->empty() && counterpart && !counterpart->empty() && ApiListHas(A.value("preserved_scripts", py::Json::array()), Norm(path)))
        {
            std::set<size_t> headers;
            for (const auto& [_, fn] : script->functions)
                headers.insert(fn.start);
            for (const auto& ref : References(tokens))
            {
                if (headers.count(ref.index) || ref.qualifier || !glue->contains(Lower(ref.name)) || !script->Find(Lower(ref.name)))
                    continue;
                tokens[ref.index].text = *counterpart + "::" + ref.name;
                tokens[ref.index].kind = Kind::Punct;
                if (ref.pointer && At(tokens, static_cast<long long>(ref.index) - 1).text == "::")
                    At(tokens, static_cast<long long>(ref.index) - 1).text = "";
                report.rewrites["glue " + ref.name + " -> " + *counterpart] += 1;
            }
        }
        for (const auto& [_, fn] : script->functions)
            if (fn.animtree)
            {
                const auto names = AnimationNames(SliceCopy(tokens, static_cast<long long>(fn.start), static_cast<long long>(fn.bodyClose) + 1));
                animationRequirements[Lower(*fn.animtree)].insert(names.begin(), names.end());
            }
        report.rewrites["stationary FX carriers lowered to authored-position FX"] += LowerStationaryFxCarriers(tokens);
        SourceLowerings(tokens);
        report.rewrites["launch volume includes its activation trigger"] += BridgeLaunchTriggers(tokens);
        const auto* originText = sources.origin->Find(Norm(path) + ".gsc");
        const std::string whereFile = originText ? *originText : path;
        std::set<std::string> includedBo2;
        std::vector<std::string> newIncludes;
        for (auto& t : tokens)
        {
            if (t.kind != Kind::Directive || !StartsWith(Lower(t.text), "#include"))
                continue;
            const auto m = IncludeRe().MatchAt(t.text);
            if (!m)
                throw std::runtime_error("'NoneType' object has no attribute 'group'");
            const auto inc = Norm((*m)[1]);
            if (bo2StockPerks && IsPerkFramework(inc))
            {
                t.text = "#include maps\\mp\\zombies\\_zm_perks;";
                includedBo2.insert("maps\\mp\\zombies\\_zm_perks");
                continue;
            }
            if (IsCore(inc, sources))
            {
                const auto mapped = ApiString(A["core_scripts"], inc);
                if (mapped && !mapped->empty() && !includedBo2.count(*mapped) && std::find(newIncludes.begin(), newIncludes.end(), *mapped) == newIncludes.end())
                {
                    newIncludes.push_back(*mapped);
                    includedBo2.insert(*mapped);
                    t.text = "#include " + *mapped + ";";
                }
                else
                    t.text = "// waw2bo2: #include " + inc + "; (WaW framework, no BO2 include)";
            }
            else if (!sources.Get(inc))
            {
                report.errors.push_back(path + ": include " + inc + " not found in the map or stock scripts");
                t.text = "// waw2bo2: #include " + inc + "; (script not found)";
            }
            else
            {
                Want(inc);
                t.text = "#include " + PortedPath(inc) + ";";
            }
        }
        animNeutralized = false;
        std::vector<std::pair<long long, long long>> neutralizedRanges;
        if (script->animtree)
        {
            for (auto& token : tokens)
            {
                if (token.kind != Kind::Directive)
                    continue;
                const auto m = AnimtreeRe().MatchAt(token.text);
                if (!m)
                    continue;
                const auto tree = Lower((*m)[1]);
                report.animtrees[(*m)[1]].push_back(path);
                if (coreAnimtrees.count(tree))
                    token.text = "#using_animtree( \"" + AnimationTreeName(tree) + "\" );";
                else if (!animtrees.count(tree))
                    token.text = "// waw2bo2: " + token.text + " (animtree not converted)";
            }
            for (const auto& [_, fn] : script->functions)
            {
                const auto tree = Lower(fn.animtree.value_or(""));
                const auto a = static_cast<long long>(fn.start), b = static_cast<long long>(fn.bodyClose) + 1;
                if (coreAnimtrees.count(tree))
                    RewriteAnimationRefs(tokens, tree, path, a, b);
                else if (!tree.empty() && !animtrees.count(tree))
                {
                    OnSlice(tokens, a, b, [&](Tokens& part) { NeutralizeAnimations(part, tree); });
                    neutralizedRanges.emplace_back(a, b);
                }
            }
        }
        if (mainSplit)
            SplitMain(*script);
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
        report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += KeepBo2Deathanims(tokens);
        std::set<size_t> defs;
        for (const auto& [_, fn] : script->functions)
            defs.insert(fn.start);
        for (const auto& [name, fn] : script->functions)
            if (ShadowsBuiltin(name))
            {
                auto& head = At(tokens, static_cast<long long>(fn.start));
                head.text = ShadowedName(head.text);
                report.rewrites["map functions named like T6 builtins renamed (waw_*)"] += 1;
            }
        ResolveRefs(tokens, *script, whereFile, includedBo2, defs, std::nullopt, &neutralizedRanges);
        report.rewrites["WaW POI record comparisons use positions on T6"] += BridgePoiRecordComparisons(tokens);
        report.rewrites["WaW contact-kill monitors own AI attacks"] += BridgeContactKillAttacks(tokens, *script);
        report.rewrites["WaW being_revived reads bridged to T6 revive state"] += BridgeReviveReads(tokens);
        report.rewrites["WaW session stat reads bridged to T6 counters"] += BridgePlayerStatReads(tokens);
        report.rewrites["WaW is_drinking flag bridged to BO2's drink counter"] += BridgeDrinkingFlag(tokens);
        report.rewrites["WaW score writes keep T6's persistent score"] += BridgeScoreWrites(tokens);
        report.rewrites["WaW HUD network font scales decoded"] += BridgeFontScales(tokens);
        report.rewrites["run-time HUD material names -> converted table"] += BridgeDynamicHudMaterials(tokens);
        report.rewrites["WaW team-held zombie vars mirrored to BO2's teams"] += MirrorTeamZombieVars(tokens);
        const std::string header = "// Translated from World at War by waw2bo2 (gscport). Source: " + whereFile + "\n";
        return header + GuardSizeLoops(Emit(tokens));
    }

    void Translator::AdaptNativeArguments(Tokens& tokens, const Ref& ref, const std::string& native, const std::optional<int> argc, const std::string& where)
    {
        const auto* adapter = ApiGet(Api()["native_argument_adapters"], native);
        if (!adapter || !argc || *argc != (*adapter)["waw_args"].get<int>())
            return;
        const auto open = static_cast<long long>(ref.index) + 1;
        const auto close = static_cast<long long>(Match(tokens, static_cast<size_t>(open), "(", ")"));
        std::vector<std::pair<long long, long long>> spans;
        long long start = open + 1;
        int depth = 0;
        for (long long j = open + 1; j < close; j++)
        {
            depth += In(At(tokens, j).text, {"(", "["});
            depth -= In(At(tokens, j).text, {")", "]"});
            if (At(tokens, j).text == "," && depth == 0)
            {
                spans.emplace_back(start, j);
                start = j + 1;
            }
        }
        spans.emplace_back(start, close);
        for (long long j = open + 1; j < close; j++)
            if (At(tokens, j).kind == Kind::Ident && At(tokens, j + 1).text == "(")
            {
                report.Unsupported(FALLBACK, where + ": " + native + " arguments call functions; not adapted");
                return;
            }
        const auto text = [&](const std::pair<long long, long long>& s)
        {
            std::vector<std::string> parts;
            const auto [lo, hi] = Slice(tokens.size(), s.first, s.second);
            for (size_t k = lo; k < hi; k++)
                parts.push_back(tokens[k].text);
            return Replace(Join(parts, " "), " . ", ".");
        };
        std::vector<std::string> out;
        for (const auto& arg : (*adapter)["args"])
        {
            if (arg.is_number_integer())
                out.push_back(text(spans.at(arg.get<size_t>())));
            else if (arg == "object")
            {
                long long k = (ref.qualifier ? static_cast<long long>(*ref.qualIndex) : static_cast<long long>(ref.index)) - 1;
                if (At(tokens, k).Low() == "thread")
                    k--;
                if (At(tokens, k).kind != Kind::Ident || !In(At(tokens, k - 1).text, {";", "{", "}"}))
                {
                    report.Unsupported(FALLBACK, where + ": " + native + " on a compound object; not adapted");
                    return;
                }
                out.push_back(At(tokens, k).text);
            }
            else if (StartsWith(arg.get<std::string>(), "owner:"))
            {
                const auto& s = spans.at(std::stoul(arg.get<std::string>().substr(6)));
                if (s.second - s.first < 3 || At(tokens, s.second - 2).text != ".")
                {
                    report.Unsupported(FALLBACK, where + ": " + native + " chunks without an owner; not adapted");
                    return;
                }
                out.push_back(text({s.first, s.second - 2}));
            }
            else if (StartsWith(arg.get<std::string>(), "defined_if:"))
            {
                const auto value = Lower(text(spans.at(std::stoul(arg.get<std::string>().substr(11)))));
                if (!In(value, {"true", "false", "1", "0"}))
                {
                    report.Unsupported(FALLBACK, where + ": " + native + " flag " + value + "; not adapted");
                    return;
                }
                out.push_back(In(value, {"true", "1"}) ? "1" : "undefined");
            }
            else
                out.push_back(arg.get<std::string>());
        }
        for (long long j = open + 1; j < close; j++)
            At(tokens, j).text = "";
        At(tokens, open).text = "( " + Join(out, ", ") + " ";
        report.rewrites[native + ": WaW arguments adapted"] += 1;
    }

    void Translator::ResolveRefs(Tokens& tokens, const Script& script, const std::string& whereFile, const std::set<std::string>& includedBo2,
                                 const std::set<size_t>& skip, const std::optional<std::string>& extracting,
                                 const std::vector<std::pair<long long, long long>>* neutralizedRanges)
    {
        const auto& A = Api();
        for (long long i = 2; i < static_cast<long long>(tokens.size()); i++)
        {
            if (At(tokens, i).Low() == "playername" && At(tokens, i - 1).text == "." && (i + 1 == static_cast<long long>(tokens.size()) || At(tokens, i + 1).text != "("))
            {
                At(tokens, i).text = "name";
                report.rewrites["WaW playername field -> T6 name"] += 1;
            }
            if (Lower(At(tokens, i).text) == "zombie_hints" && At(tokens, i - 1).text == "." && Lower(At(tokens, i - 2).text) == "level")
                At(tokens, i).text = "waw2bo2_hints";
        }
        report.rewrites["zombie_weapons key lists without BO2-only entries"] += LowerZombieWeaponKeys(tokens);
        for (const auto& ref : References(tokens))
        {
            const auto index = static_cast<long long>(ref.index);
            if (neutralizedRanges)
            {
                animNeutralized = false;
                for (const auto& [a, b] : *neutralizedRanges)
                    animNeutralized = animNeutralized || (a <= index && index < b);
            }
            if (skip.count(ref.index) || tokens[ref.index].text.empty())
                continue;
            auto& tok = tokens[ref.index];
            const auto name = Lower(ref.name);
            const auto where = whereFile + ":" + std::to_string(tok.line);
            std::optional<int> argc = ref.pointer ? std::nullopt : Argc(tokens, index);
            if (!ref.qualifier && argc == 2 && !Owner(script, name) && In(name, {"getent", "getentarray"}))
            {
                int depth = 0;
                for (long long end = index + 1; end < static_cast<long long>(tokens.size()); end++)
                {
                    if (At(tokens, end).text == "(")
                        depth++;
                    else if (At(tokens, end).text == ")")
                    {
                        depth--;
                        if (depth == 0)
                        {
                            const auto field = Lower(StripChars(At(tokens, end - 1).text, "\""));
                            const auto* bridges = ApiGet(A.value("entity_lookup_bridges", py::Json::object()), field);
                            const auto bridge = bridges ? ApiString(*bridges, name) : std::nullopt;
                            if (bridge && !bridge->empty() && At(tokens, end - 1).kind == Kind::String && StartsWith(At(tokens, end - 1).text, "\"")
                                && At(tokens, end - 2).text == ",")
                            {
                                tok.text = std::string(COMPAT) + "::" + *bridge;
                                report.rewrites["WaW entity lookup field " + field] += 1;
                            }
                            break;
                        }
                    }
                }
                if (tok.text != ref.name)
                    continue;
            }
            const std::optional<std::string> perkOwner = ref.qualifier ? std::optional<std::string>(Norm(*ref.qualifier)) : Owner(script, name);
            const auto parts = perkOwner ? wawPerkParts.find(*perkOwner) : wawPerkParts.end();
            if (bo2StockPerks && parts != wawPerkParts.end() && parts->second.count(name))
            {
                if (!ref.qualifier && perkOwner != script.path)
                {
                    Want(*perkOwner);
                    if (ref.pointer)
                        At(tokens, index - 1).text = "";
                    tok.text = PortedPath(*perkOwner) + "::" + ref.name;
                    continue;
                }
            }
            else if (bo2StockPerks && IsPerkFramework(perkOwner))
            {
                const std::string native = "maps\\mp\\zombies\\_zm_perks";
                std::string repl;
                if (name == "init")
                    repl = std::string(COMPAT) + "::stock_perks_noop";
                else if (In(name, {"perk_hud_create", "perk_think"}))
                    repl = std::string(COMPAT) + "::stock_perks_owned_by_bo2";
                else if (name == "regret_purchase")
                    repl = std::string(COMPAT) + "::stock_perks_no_refund";
                else if (name == "play_no_money_perk_dialog")
                    repl = std::string(COMPAT) + "::stock_perks_no_money";
                else if (name == "perk_vo")
                    repl = "maps\\mp\\zombies\\_zm_audio::perk_vox";
                else if (api.Defines(native, name))
                    repl = native + "::" + name;
                else
                {
                    report.errors.push_back(where + ": stock BO2 perks have no equivalent for " + *perkOwner + "::" + name);
                    repl = Stub(*perkOwner + "::" + name, name, argc, where);
                }
                if (ref.qualifier)
                {
                    tokens[*ref.qualIndex].text = "";
                    tokens[*ref.qualIndex + 1].text = "";
                }
                else if (ref.pointer)
                    At(tokens, index - 1).text = "";
                tok.text = repl;
                continue;
            }
            if (ref.qualifier)
            {
                const auto target = Norm(*ref.qualifier);
                auto& qualTok = tokens[*ref.qualIndex];
                if (const auto library = LibraryFunction(target, name))
                {
                    qualTok.text = "";
                    tokens[*ref.qualIndex + 1].text = "";
                    tok.text = *library;
                }
                else if (IsCore(target, sources))
                {
                    auto repl = ResolveCore(target, name, argc, where, {}, extracting && target == *extracting);
                    if (!repl)
                    {
                        // f"{API['core_scripts'][target]}::{name}": KeyError if absent, "None" for null
                        const auto& mapped = A["core_scripts"].at(target);
                        repl = (mapped.is_null() ? std::string("None") : mapped.get<std::string>()) + "::" + ref.name;
                    }
                    if (!ref.pointer)
                        AdaptNativeArguments(tokens, ref, *repl, argc, where);
                    tokens[*ref.qualIndex].text = "";
                    tokens[*ref.qualIndex + 1].text = "";
                    tokens[ref.index].text = *repl;
                }
                else
                {
                    const auto dep = sources.Get(target);
                    if (!dep || !dep->Find(name))
                    {
                        qualTok.text = "";
                        tokens[*ref.qualIndex + 1].text = "";
                        tok.text = Stub(target + "::" + name, name, argc, where);
                    }
                    else
                    {
                        Want(target);
                        qualTok.text = PortedPath(target);
                        if (ShadowsBuiltin(name))
                            tok.text = ShadowedName(tok.text);
                    }
                }
                continue;
            }
            const auto owner = Owner(script, name);
            if (owner && !IsCore(*owner, sources) && !extracting && ShadowsBuiltin(name))
            {
                tok.text = ShadowedName(tok.text);
                continue;
            }
            const auto library = owner ? LibraryFunction(*owner, name) : std::nullopt;
            if (library && *owner != script.path)
            {
                if (ref.pointer)
                    At(tokens, index - 1).text = "";
                tok.text = *library;
                continue;
            }
            if (owner && *owner == script.path && !extracting)
                continue;
            std::optional<std::string> repl;
            if (owner && !IsCore(*owner, sources))
            {
                if (!extracting)
                    continue;
                Want(*owner);
                repl = PortedPath(*owner) + "::" + ref.name;
            }
            else if (owner)
                repl = ResolveCore(*owner, name, argc, where, includedBo2, extracting && *owner == *extracting);
            else
            {
                const std::optional<bool> method = ref.pointer ? std::nullopt : std::optional<bool>(ref.method);
                const auto range = api.BuiltinRange(name, method);
                if (range && argc && *argc > range->hi && !CompatName(name))
                {
                    TruncateArgs(tokens, index, range->hi);
                    report.rewrites["builtin " + name + ": " + std::to_string(*argc - range->hi) + " WaW-only argument(s) dropped"] += 1;
                    argc = range->hi;
                }
                repl = ResolveBuiltin(name, argc, where, includedBo2, method);
            }
            std::optional<std::string> nativeCleanup = repl;
            if (!repl && owner)
                if (const auto mapped = ApiString(A["core_scripts"], *owner); mapped && !mapped->empty())
                    nativeCleanup = *mapped + "::" + name;
            if (nativeCleanup && !nativeCleanup->empty() && !ref.pointer)
                AdaptNativeArguments(tokens, ref, *nativeCleanup, argc, where);
            const auto wrapper = nativeCleanup ? ApiString(A["hud_cleanup_wrappers"], *nativeCleanup) : std::nullopt;
            if (wrapper && !wrapper->empty() && ref.method && argc == 0)
                if (BridgeHudCleanup(tokens, index, *wrapper))
                {
                    report.rewrites["idempotent native HUD cleanup"] += 1;
                    continue;
                }
            if (repl)
            {
                if (extracting && StartsWith(*repl, std::string(CORE) + "::"))
                    repl = repl->substr(std::string(CORE).size() + 2);
                if (ref.pointer)
                {
                    At(tokens, index - 1).text = "";
                    tokens[ref.index].text = Contains(*repl, "::") ? *repl : "::" + *repl;
                }
                else
                    tokens[ref.index].text = *repl;
            }
        }
    }

    void Translator::RewriteAnimationRefs(Tokens& tokens, const std::string& tree, const std::string& where, const long long start,
                                          const std::optional<long long> end)
    {
        const auto& available = coreAnimtrees.at(Lower(tree));
        const long long stop = end.value_or(static_cast<long long>(tokens.size())) - 1;
        for (long long i = std::max(start, 1LL); i < stop; i++)
        {
            if (At(tokens, i).text != "%" || At(tokens, i + 1).kind != Kind::Ident || !In(At(tokens, i - 1).text, {"(", ",", "=", "[", "return", "[["}))
                continue;
            const auto name = At(tokens, i + 1).Low();
            if (available.count(name))
                At(tokens, i + 1).text = AnimationAssetName(name);
            else
            {
                report.Unsupported("XANIM " + name + " (animtree " + tree + ": no WaW xanim)", where);
                At(tokens, i).text = "";
                At(tokens, i + 1).text = "undefined";
            }
        }
    }

    void Translator::KeepCoreAnimations(const Script& script, Tokens& tokens, const std::string& where, const std::optional<std::string>& animtree)
    {
        const auto tree = Lower(animtree && !animtree->empty() ? *animtree : script.animtree.value_or(""));
        bool usesTree = false;
        for (size_t i = 0; i + 1 < tokens.size(); i++)
            usesTree = usesTree || (tokens[i].text == "#" && tokens[i + 1].Low() == "animtree");
        if (!coreAnimtrees.count(tree) || !(usesTree || UsesAnimations(tokens, 0, static_cast<long long>(tokens.size()) - 1)))
            return;
        tokens[0].pre += "#using_animtree( \"" + AnimationTreeName(tree) + "\" );\n";
        RewriteAnimationRefs(tokens, tree, where);
    }

    std::string Translator::GuardSizeLoops(const std::string& text)
    {
        auto tokens = Tokenize(text);
        const auto changed = LowerUndefinedSizeLoops(tokens);
        report.rewrites["loops over a .size that may be undefined"] += changed;
        return changed ? Emit(tokens) : text;
    }

    void Translator::SourceLowerings(Tokens& tokens)
    {
        report.rewrites["maxhealth assignments through setmaxhealth for players"] += LowerMaxhealthAssignments(tokens);
        report.rewrites["health assignments set a player's cap (WaW field setter)"] += LowerHealthAssignments(tokens);
        report.rewrites["zombie_weapons key lists without BO2-only entries"] += LowerZombieWeaponKeys(tokens);
        report.rewrites["HUD fields (safe-area anchors, hidden in menus)"] += LowerHudFields(tokens);
        report.rewrites["integer literals past 32 bits wrapped as WaW reads them"] += WrapIntegerLiterals(tokens);
    }

    std::string Translator::TranslateExtracted(const std::string& core, const std::string& name)
    {
        const auto script = sources.Get(core);
        const Function& fn = *script->Find(name);
        if (fn.animtree)
        {
            const auto names = AnimationNames(SliceCopy(script->tokens, static_cast<long long>(fn.start), static_cast<long long>(fn.bodyClose) + 1));
            animationRequirements[Lower(*fn.animtree)].insert(names.begin(), names.end());
        }
        Tokens tokens = SliceCopy(script->tokens, static_cast<long long>(fn.start), static_cast<long long>(fn.bodyClose) + 1);
        tokens.push_back(Token{Kind::Eof, "", "", 0});
        tokens[0].text = coreFuncs.at({core, name});
        animNeutralized = false;
        tokens[0].pre = "\n// " + core + "::" + name + " (" + (sources.text->Has(core + ".gsc") ? "map override" : "stock") + ")\n";
        KeepCoreAnimations(*script, tokens, core + "::" + name, fn.animtree);
        const auto tree = Lower(fn.animtree.value_or(""));
        bool usesTree = false;
        for (size_t i = 0; i + 1 < tokens.size(); i++)
            usesTree = usesTree || (tokens[i].text == "#" && tokens[i + 1].Low() == "animtree");
        if (usesTree && !coreAnimtrees.count(tree))
        {
            if (animtrees.count(tree))
                tokens[0].pre += "#using_animtree( \"" + fn.animtree.value_or("") + "\" );\n";
            else
                NeutralizeAnimations(tokens, fn.animtree.value_or(""));
        }
        const auto* originText = sources.origin->Find(core + ".gsc");
        const std::string where = originText ? *originText : core;
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        SourceLowerings(tokens);
        report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
        report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += KeepBo2Deathanims(tokens);
        ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, core);
        report.rewrites["WaW HUD network font scales decoded"] += BridgeFontScales(tokens);
        report.rewrites["WaW POI record comparisons use positions on T6"] += BridgePoiRecordComparisons(tokens);
        report.rewrites["run-time HUD material names -> converted table"] += BridgeDynamicHudMaterials(tokens);
        report.rewrites["WaW team-held zombie vars mirrored to BO2's teams"] += MirrorTeamZombieVars(tokens);
        if (intermissionFuncs.count({core, name}))
            report.rewrites["intermission player link -> T6 script camera"] += BridgeIntermissionCamera(tokens);
        auto text = Emit(tokens);
        const auto patch = postPatches.find({core, name});
        return GuardSizeLoops(patch != postPatches.end() ? patch->second(text) : text);
    }
} // namespace w2b::gsc::port

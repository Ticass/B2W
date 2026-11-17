// Port of the Translator extraction methods (gscport.py 1958-2552, 2636-2879).
#include "translator.h"

#include "api.h"
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

        std::string Prefix(const std::string& core)
        {
            const auto slash = core.rfind('\\');
            auto last = slash == std::string::npos ? core : core.substr(slash + 1);
            return last.substr(std::min(last.find_first_not_of('_'), last.size()));
        }

        Token Tok(const Kind kind, std::string text, std::string pre = "")
        {
            return Token{kind, std::move(text), std::move(pre), 0};
        }

        std::vector<std::string> Lows(const Tokens& tokens, const long long a, const long long b)
        {
            std::vector<std::string> out;
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            for (size_t k = lo; k < hi; k++)
                out.push_back(tokens[k].Low());
            return out;
        }

        std::string Origin(Sources& sources, const std::string& key, const std::string& fallback)
        {
            const auto* found = sources.origin->Find(key);
            return found ? *found : fallback;
        }

        const std::vector<std::pair<std::string, std::string>>& Anchors()
        {
            static const std::vector<std::pair<std::string, std::string>> anchors{
                {"spawn", R"(level\s+thread\s+\[\[\s*(?:level\.)?round_spawn_func\s*\]\]\s*\(\s*\)\s*;)"},
                {"wait", R"(\bROUND_WAIT\s*\(\s*\)\s*;)"},
                {"advance", R"(level\.(\w*round\w*)\s*\+\+\s*;)"}};
            return anchors;
        }

        std::pair<std::string, std::string> GlueCalls(const std::string& label)
        {
            if (label == "spawn")
                return {"waw2bo2_round_spawn_ready", "waw2bo2_round_started"};
            if (label == "wait")
                return {"", "waw2bo2_round_waited"};
            return {"", "waw2bo2_round_advanced"};
        }
    } // namespace

    std::optional<std::string> Translator::Extract(const std::string& core, const std::string& name)
    {
        if (const auto it = coreFuncs.find({core, name}); it != coreFuncs.end())
            return std::string(CORE) + "::" + it->second;
        const auto& A = Api();
        if (ApiListHas(A["no_extract"], core) && !SourceDrives(core, name))
        {
            bool excepted = false;
            if (const auto* exceptions = ApiGet(A["extract_exceptions"], core))
                for (const auto& s : *exceptions)
                    excepted = excepted || EndsWith(name, s.get<std::string>());
            if (!excepted)
                return std::nullopt;
        }
        const auto script = sources.Get(core);
        if (!script || !script->Find(name))
            return std::nullopt;
        const Function& fn = *script->Find(name);
        const auto tree = Lower(fn.animtree.value_or(""));
        if (!tree.empty())
        {
            const auto names = AnimationNames(SliceCopy(script->tokens, static_cast<long long>(fn.start), static_cast<long long>(fn.bodyClose) + 1));
            animationRequirements[tree].insert(names.begin(), names.end());
        }
        if (UsesAnimations(script->tokens, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)) && !coreAnimtrees.count(tree)
            && !animationDiscovery)
        {
            report.animtrees[fn.animtree && !fn.animtree->empty() ? *fn.animtree : "?"].push_back(core + "::" + name);
            return std::nullopt;
        }
        coreFuncs[{core, name}] = Prefix(core) + "__" + name;
        coreQueue.emplace_back(core, name);
        report.extracted.push_back(core + "::" + name);
        return std::string(CORE) + "::" + coreFuncs[{core, name}];
    }

    std::optional<std::string> Translator::ExtractEntry(const std::string& core, const std::string& name)
    {
        const auto script = sources.Get(core);
        if (!script || !script->Find(name))
            return std::nullopt;
        const std::pair<std::string, std::string> key{core, name};
        if (!coreFuncs.count(key))
        {
            coreFuncs[key] = Prefix(core) + "__" + name;
            coreQueue.push_back(key);
            report.extracted.push_back(core + "::" + name);
        }
        return coreFuncs[key];
    }

    std::optional<std::string> Translator::ExtractLevelState(const std::string& core, const std::string& init, const std::optional<std::set<std::string>>& readIn,
                                                             const std::optional<std::set<std::string>>& effectKeys)
    {
        const auto script = sources.Get(core);
        if (!script || !script->Find(init))
            return std::nullopt;
        std::set<std::string> read;
        if (readIn)
            read = *readIn;
        else
            for (const auto& [key, _] : coreFuncs)
            {
                const Function* fn = key.first == core ? script->Find(key.second) : nullptr;
                if (!fn)
                    continue;
                const auto fields = LevelFieldsRead(script->tokens, static_cast<long long>(fn->bodyOpen) + 2, static_cast<long long>(fn->bodyClose));
                read.insert(fields.begin(), fields.end());
            }
        const Function fn = *script->Find(init);
        const auto& toks = script->tokens;
        const auto spans = TopLevelStatements(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose));
        std::set<size_t> keep;
        for (size_t n = 0; n < spans.size(); n++)
        {
            const auto stmt = SliceCopy(toks, spans[n].first, spans[n].second);
            if (!effectKeys)
            {
                const auto field = GuardedLevelDefault(stmt);
                if (field && read.count(*field))
                {
                    keep.insert(n);
                    continue;
                }
            }
            if (effectKeys
                && !(stmt.size() > 5 && stmt[3].text == "[" && stmt[4].kind == Kind::String && stmt[4].text.size() >= 2
                     && effectKeys->count(Lower(stmt[4].text.substr(1, stmt[4].text.size() - 2)))))
                continue;
            bool assigns = false;
            for (const auto& s : stmt)
                assigns = assigns || s.text == "=";
            if (stmt.size() > 3 && stmt[0].Low() == "level" && stmt[1].text == "." && read.count(stmt[2].Low()) && assigns)
                keep.insert(n);
        }
        const auto extra = LocalDependencies(toks, spans, keep);
        keep.insert(extra.begin(), extra.end());
        Tokens kept;
        for (const auto n : keep)
            for (const auto& s : SliceCopy(toks, spans[n].first, spans[n].second))
                kept.push_back(Token{s.kind, s.text, s.pre, s.line});
        if (kept.empty())
            return std::nullopt;
        const auto base = Prefix(core) + "__" + init + "_level_state";
        auto name = base;
        int n = 1;
        while (levelStateNames.count(name))
        {
            n++;
            name = base + "_" + std::to_string(n);
        }
        levelStateNames.insert(name);
        Tokens tokens{Tok(Kind::Ident, name, "\n// " + core + "::" + init + ": level state its extracted functions read\n"), Tok(Kind::Punct, "("),
                      Tok(Kind::Punct, ")"), Tok(Kind::Punct, "{", "\n")};
        tokens.insert(tokens.end(), kept.begin(), kept.end());
        tokens.push_back(Tok(Kind::Punct, "}", "\n"));
        tokens.push_back(Tok(Kind::Eof, ""));
        if (fn.animtree)
        {
            const auto names = AnimationNames(tokens);
            animationRequirements[Lower(*fn.animtree)].insert(names.begin(), names.end());
        }
        KeepCoreAnimations(*script, tokens, core + "::" + init, fn.animtree);
        const auto tree = Lower(fn.animtree.value_or(""));
        if (!tree.empty() && !coreAnimtrees.count(tree) && !animtrees.count(tree))
            NeutralizeAnimations(tokens, *fn.animtree);
        const auto where = Origin(sources, core + ".gsc", core);
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
        report.rewrites["WaW deathanim assignments kept BO2's (ASD state)"] += KeepBo2Deathanims(tokens);
        ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, core);
        report.extracted.push_back(core + "::" + init + " (level state)");
        levelStateParts.push_back(Emit(tokens));
        return name;
    }

    std::optional<std::string> Translator::ExtractSpawnerLists()
    {
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* fnp = script ? script->Find("main") : nullptr;
        if (!fnp)
            return std::nullopt;
        const Function fn = *fnp;
        const auto& toks = script->tokens;
        Tokens kept;
        for (const auto& [start, end] : TopLevelStatements(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
        {
            const auto stmt = SliceCopy(toks, start, end);
            const long long len = static_cast<long long>(stmt.size());
            std::set<std::string> targets, calls;
            for (long long k = 0; k < len - 3; k++)
                if (stmt[static_cast<size_t>(k)].Low() == "level" && stmt[static_cast<size_t>(k + 1)].text == "." && stmt[static_cast<size_t>(k + 3)].text == "=")
                    targets.insert(stmt[static_cast<size_t>(k + 2)].Low());
            for (long long k = 0; k < len - 1; k++)
                if (stmt[static_cast<size_t>(k)].kind == Kind::Ident && stmt[static_cast<size_t>(k + 1)].text == "("
                    && !In(stmt[static_cast<size_t>(k)].Low(), {"if", "else", "while", "for"}))
                    calls.insert(stmt[static_cast<size_t>(k)].Low());
            const bool listsOnly = std::all_of(targets.begin(), targets.end(), [](const std::string& t) { return t == "enemy_spawns" || t == "zombie_rise_spawners"; });
            const bool callsOnly = std::all_of(calls.begin(), calls.end(), [](const std::string& c) { return c == "isdefined" || c == "getentarray"; });
            if (!targets.empty() && listsOnly && callsOnly)
                for (const auto& s : stmt)
                    kept.push_back(Token{s.kind, s.text, s.pre, s.line});
        }
        if (kept.empty())
            return std::nullopt;
        const std::string name = "zombiemode__main_spawner_lists";
        Tokens tokens{Tok(Kind::Ident, name, std::string("\n// ") + ZOMBIEMODE + "::main: the spawner lists its spawn loop and zones read\n"), Tok(Kind::Punct, "(")};
        for (size_t k = 0; k < fn.params.size(); k++)
        {
            if (k)
                tokens.push_back(Tok(Kind::Punct, ","));
            tokens.push_back(Tok(Kind::Ident, fn.params[k], " "));
        }
        tokens.push_back(Tok(Kind::Punct, ")", " "));
        tokens.push_back(Tok(Kind::Punct, "{", "\n"));
        tokens.insert(tokens.end(), kept.begin(), kept.end());
        tokens.push_back(Tok(Kind::Punct, "}", "\n"));
        tokens.push_back(Tok(Kind::Eof, ""));
        const auto where = Origin(sources, std::string(ZOMBIEMODE) + ".gsc", ZOMBIEMODE);
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
        ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, std::string(ZOMBIEMODE));
        report.extracted.push_back(std::string(ZOMBIEMODE) + "::main (spawner lists)");
        levelStateParts.push_back(Emit(tokens));
        return name;
    }

    std::optional<std::string> Translator::ExtractEndGameMusic()
    {
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* fnp = script ? script->Find("end_game") : nullptr;
        if (!fnp)
            return std::nullopt;
        const Function fn = *fnp;
        const auto& toks = script->tokens;
        Tokens kept;
        for (const auto& [start, end] : TopLevelStatements(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
        {
            const auto stmt = SliceCopy(toks, start, end);
            std::set<std::string> calls;
            for (size_t k = 0; k + 1 < stmt.size(); k++)
                if (stmt[k].kind == Kind::Ident && stmt[k + 1].text == "(")
                    calls.insert(stmt[k].Low());
            if (!calls.empty() && std::all_of(calls.begin(), calls.end(), [](const std::string& c) { return c == "setmusicstate" || c == "setbusstate"; }))
                for (const auto& s : stmt)
                    kept.push_back(Token{s.kind, s.text, s.pre, s.line});
        }
        if (kept.empty())
            return std::nullopt;
        const std::string name = "zombiemode__end_game_music";
        Tokens tokens{Tok(Kind::Ident, name, std::string("\n// ") + ZOMBIEMODE + "::end_game: the music it starts\n"), Tok(Kind::Punct, "("), Tok(Kind::Punct, ")"),
                      Tok(Kind::Punct, "{", "\n")};
        tokens.insert(tokens.end(), kept.begin(), kept.end());
        tokens.push_back(Tok(Kind::Punct, "}", "\n"));
        tokens.push_back(Tok(Kind::Eof, ""));
        const auto where = Origin(sources, std::string(ZOMBIEMODE) + ".gsc", ZOMBIEMODE);
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, std::string(ZOMBIEMODE));
        report.extracted.push_back(std::string(ZOMBIEMODE) + "::end_game (music)");
        levelStateParts.push_back(Emit(tokens));
        return name;
    }

    std::optional<std::string> Translator::ExtractPlayerConnectFields()
    {
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* fnp = script ? script->Find("onplayerconnect") : nullptr;
        if (!fnp)
            return std::nullopt;
        const Function fn = *fnp;
        const auto& toks = script->tokens;
        std::string loopVar;
        Tokens kept;
        for (long long i = static_cast<long long>(fn.bodyOpen); i < static_cast<long long>(fn.bodyClose) - 6; i++)
        {
            if (!(WordsEqual(toks, i, {"level", "waittill", "(", "\"connecting\""}) && At(toks, i + 4).text == "," && At(toks, i + 5).kind == Kind::Ident))
                continue;
            loopVar = At(toks, i + 5).Low();
            const auto start = static_cast<long long>(Match(toks, static_cast<size_t>(i + 2), "(", ")")) + 2;
            long long end = -1;
            for (long long k = start; k < static_cast<long long>(fn.bodyClose); k++)
                if (At(toks, k).text == "}")
                {
                    end = k;
                    break;
                }
            if (end < 0)
                throw std::runtime_error("StopIteration");
            for (const auto& [a, b] : TopLevelStatements(toks, start - 1, end))
            {
                const auto stmt = SliceCopy(toks, a, b);
                bool waits = false;
                for (const auto& t : stmt)
                    waits = waits || In(t.Low(), {"thread", "waittill", "wait"});
                if (stmt.size() > 4 && stmt[0].Low() == loopVar && stmt[1].text == "." && stmt[3].text == "=" && !waits)
                    for (const auto& s : stmt)
                        kept.push_back(Token{s.kind, s.text, s.pre, s.line});
            }
            break;
        }
        if (kept.empty())
            return std::nullopt;
        const std::string name = "zombiemode__onplayerconnect_fields";
        auto body = Tokenize(name + "()\n{\n\tfor ( ;; )\n\t{\n\t\tlevel waittill( \"connecting\", " + loopVar + " );\n");
        body.pop_back();
        body[0].pre = std::string("\n// ") + ZOMBIEMODE + "::onPlayerConnect: the player fields its scripts read\n";
        Tokens tokens = body;
        tokens.insert(tokens.end(), kept.begin(), kept.end());
        const auto tail = Tokenize("\n\t}\n}\n");
        tokens.insert(tokens.end(), tail.begin(), tail.end());
        const auto where = Origin(sources, std::string(ZOMBIEMODE) + ".gsc", ZOMBIEMODE);
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
        report.rewrites["WaW score writes keep T6's persistent score"] += BridgeScoreWrites(tokens);
        ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, std::string(ZOMBIEMODE));
        report.extracted.push_back(std::string(ZOMBIEMODE) + "::onplayerconnect (player fields)");
        levelStateParts.push_back(Emit(tokens));
        return name;
    }

    std::optional<std::string> Translator::ExtractDamagePrelude(const std::string& coreIn, const std::string& functionIn)
    {
        const std::string core = coreIn.empty() ? ZOMBIEMODE : coreIn;
        const std::string function = functionIn.empty() ? DAMAGE_OVERRIDE : functionIn;
        const auto key = core + ".gsc";
        const auto variants = sources.StockVariants(core);
        const auto* own = sources.text->Find(key);
        if (!own || variants.empty() || std::find(variants.begin(), variants.end(), *own) != variants.end())
            return std::nullopt;
        const auto script = sources.Get(core);
        const Function* fnp = script->Find(function);
        if (!fnp)
            return std::nullopt;
        const Function fn = *fnp;
        const auto& toks = script->tokens;
        const auto where = Origin(sources, key, core);
        const auto text = [](const Tokens& ts, const Span& span) { return Join(Lows(ts, span.first, span.second), " "); };
        const auto header = [&](const Tokens& ts, const Span& span)
        {
            const auto [start, end] = span;
            if (At(ts, start).Low() != "if" || At(ts, start + 1).text != "(")
                return text(ts, span);
            int depth = 0;
            for (long long i = start + 1; i < end; i++)
            {
                depth += At(ts, i).text == "(";
                depth -= At(ts, i).text == ")";
                if (depth == 0)
                    return text(ts, {start, i + 1});
            }
            return text(ts, span);
        };
        const auto mapSpans = TopLevelStatements(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose));
        std::vector<std::string> stmts;
        for (const auto& s : mapSpans)
            stmts.push_back(text(toks, s));
        std::vector<size_t> leads;
        for (const auto& variant : variants)
        {
            const auto stock = Parse(variant, core);
            const Function* base = stock.Find(function);
            const auto spans = base ? TopLevelStatements(stock.tokens, static_cast<long long>(base->bodyOpen), static_cast<long long>(base->bodyClose))
                                    : std::vector<Span>{};
            if (!spans.empty() && std::find(stmts.begin(), stmts.end(), text(stock.tokens, spans[0])) != stmts.end())
                leads.push_back(static_cast<size_t>(std::find(stmts.begin(), stmts.end(), text(stock.tokens, spans[0])) - stmts.begin()));
            else if (!spans.empty())
            {
                const auto anchor = header(stock.tokens, spans[0]);
                for (size_t i = 0; i < mapSpans.size(); i++)
                    if (header(toks, mapSpans[i]) == anchor)
                        leads.push_back(i);
            }
        }
        if (leads.empty())
        {
            report.Unsupported(FALLBACK, where + ": " + function + " does not keep a stock WaW body; the map's damage rules are not ported");
            return std::nullopt;
        }
        const auto lead = *std::min_element(leads.begin(), leads.end());
        if (lead == 0)
            return std::nullopt;
        std::vector<Span> rules(mapSpans.begin(), mapSpans.begin() + static_cast<long long>(lead));
        if (bo2StockPerks)
        {
            static const pyre::Regex perk(R"(\bhasperk \()");
            std::vector<Span> perkRules, rest;
            for (const auto& span : rules)
                (perk.Search(header(toks, span)) ? perkRules : rest).push_back(span);
            rules = rest;
            if (!perkRules.empty())
                report.rewrites["WaW perk damage rules dropped (BO2 stock perks)"] += static_cast<long long>(perkRules.size());
            if (rules.empty())
                return std::nullopt;
        }
        Tokens body;
        for (const auto& [a, b] : rules)
            for (const auto& t : SliceCopy(toks, a, b))
                body.push_back(t);
        for (const auto& ref : References(body))
        {
            if (ref.pointer || Lower(ref.name) != "finishplayerdamagewrapper" || Norm(ref.qualifier.value_or("")) != "maps\\_callbackglobal" || !ref.qualIndex
                || *ref.qualIndex < 1 || body[*ref.qualIndex - 1].Low() != "self")
                continue;
            const auto opening = static_cast<long long>(ref.index) + 1;
            const auto closing = static_cast<long long>(Match(body, static_cast<size_t>(opening), "(", ")"));
            std::vector<Tokens> args;
            long long begin = opening + 1;
            int depth = 0;
            for (long long i = begin; i < closing; i++)
            {
                const auto& t = At(body, i).text;
                if (t == "(" || t == "[")
                    depth++;
                else if (t == ")" || t == "]")
                    depth--;
                else if (t == "," && depth == 0)
                {
                    args.push_back(SliceCopy(body, begin, i));
                    begin = i + 1;
                }
            }
            args.push_back(SliceCopy(body, begin, closing));
            std::vector<std::string> afterText;
            for (const auto& t : SliceCopy(body, closing + 1, closing + 4))
                afterText.push_back(Lower(t.text));
            if (args.size() != 11 || args[2].size() != 1 || args[2][0].text != "0" || afterText != std::vector<std::string>{";", "return", ";"})
                continue;
            const auto start = static_cast<long long>(*ref.qualIndex) - 1;
            At(body, start).text = fn.params.at(2) + " = 0;";
            At(body, start).kind = Kind::Punct;
            const auto [lo, hi] = Slice(body.size(), start + 1, closing + 2);
            for (size_t k = lo; k < hi; k++)
                body[k].text = body[k].pre = "";
            report.rewrites["WaW zero-damage finish/return -> T6 damage cancellation"] += 1;
        }
        bool finishes = false;
        for (const auto& t : body)
            finishes = finishes || In(t.Low(), {"finishplayerdamage", "finishplayerdamagewrapper"});
        if (fn.params.size() != 11 || finishes)
        {
            report.Unsupported(FALLBACK, where + ": " + function + " prelude applies damage itself; not ported");
            return std::nullopt;
        }
        const auto name = Prefix(core) + "__" + function + "_prelude";
        std::vector<std::string> params(fn.params.begin(), fn.params.begin() + 9);
        params.insert(params.end(), fn.params.begin() + 10, fn.params.end());
        const auto modelIndex = Lower(fn.params[9]);
        bool usesModel = false;
        for (const auto& t : body)
            usesModel = usesModel || t.Low() == modelIndex;
        const std::string reset = usesModel ? "\t" + fn.params[9] + " = undefined;\n" : "";
        const auto& damage = fn.params[2];
        const std::string source = name + "( " + Join(params, ", ") + " )\n{\n" + reset + "\twaw2bo2_damage_in = " + damage + ";\n" + Emit(body) + "\n\tif ( " + damage
                                   + " != waw2bo2_damage_in )\n\t\treturn " + damage + ";\n\treturn -1;\n}\n";
        auto tokens = Tokenize(source);
        for (long long i = static_cast<long long>(tokens.size()) - 1; i > 0; i--)
            if (At(tokens, i).Low() == "return" && At(tokens, i + 1).text == ";")
                tokens.insert(tokens.begin() + (i + 1), Token{Kind::Number, "0", " ", 0});
        tokens[0].pre = "\n// " + core + "::" + function + ": the map's leading rules, as a BO2 damage callback\n";
        report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
        report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
        ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, core);
        report.extracted.push_back(core + "::" + function + " (map prelude -> damage callback)");
        levelStateParts.push_back(Emit(tokens) + "\n" + name + "_register()\n{\n\tmaps\\mp\\zombies\\_zm::register_player_damage_callback( ::" + name + " );\n}\n");
        return name + "_register";
    }

    std::optional<std::string> Translator::ExtractMainLevelState()
    {
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* mainp = script ? script->Find("main") : nullptr;
        if (!mainp)
            return std::nullopt;
        const Function main = *mainp;
        const auto& toks = script->tokens;
        const auto mainAssigned = LevelFieldsAssigned(toks, static_cast<long long>(main.bodyOpen), static_cast<long long>(main.bodyClose));
        std::set<std::string> read, elsewhere;
        for (const auto& path : sources.text->order)
        {
            const auto tokens = Tokenize(*sources.text->Find(path));
            const bool isMain = Norm(path) == ZOMBIEMODE;
            for (long long i = 2; i < static_cast<long long>(tokens.size()) - 1; i++)
                if (tokens[static_cast<size_t>(i)].kind == Kind::Ident && tokens[static_cast<size_t>(i - 1)].text == "." && tokens[static_cast<size_t>(i - 2)].Low() == "level"
                    && tokens[static_cast<size_t>(i + 1)].text != "=" && !(isMain && static_cast<long long>(main.bodyOpen) <= i && i <= static_cast<long long>(main.bodyClose)))
                    read.insert(tokens[static_cast<size_t>(i)].Low());
            if (isMain)
            {
                const auto a = LevelFieldsAssigned(tokens, 2, static_cast<long long>(main.bodyOpen));
                const auto b = LevelFieldsAssigned(tokens, static_cast<long long>(main.bodyClose));
                elsewhere.insert(a.begin(), a.end());
                elsewhere.insert(b.begin(), b.end());
            }
            else
            {
                const auto a = LevelFieldsAssigned(tokens);
                elsewhere.insert(a.begin(), a.end());
            }
        }
        std::set<std::string> defaults;
        for (const auto& [a, b] : TopLevelStatements(toks, static_cast<long long>(main.bodyOpen), static_cast<long long>(main.bodyClose)))
            if (const auto field = GuardedLevelDefault(SliceCopy(toks, a, b)))
                defaults.insert(*field);
        std::set<std::string> fields;
        for (const auto& f : read)
        {
            if ((mainAssigned.count(f) && !elsewhere.count(f)) || defaults.count(f))
                fields.insert(f);
        }
        for (const char* f : {"overrideplayerdamage", "playerlaststand_func", "player_becomes_zombie", "laststandpistol"})
            fields.insert(f);
        for (const char* f : {"global_damage_func", "global_damage_func_ads"})
            fields.erase(f);
        return ExtractLevelState(ZOMBIEMODE, "main", fields);
    }

    std::optional<std::string> Translator::ExtractSourcePlayerDamage()
    {
        const auto script = sources.Get(CALLBACKGLOBAL);
        if (!script || !script->Find("callback_playerdamage") || !script->Find("callback_playerlaststand"))
        {
            report.Unsupported(FALLBACK, std::string(CALLBACKGLOBAL) + ": no Callback_PlayerDamage/Callback_PlayerLastStand source; BO2's player damage and last stand run");
            return std::nullopt;
        }
        const auto damage = ExtractEntry(CALLBACKGLOBAL, "callback_playerdamage");
        const auto laststand = ExtractEntry(CALLBACKGLOBAL, "callback_playerlaststand");
        std::vector<std::string> lines{"\tlevel.waw2bo2_source_player_damage = ::" + damage.value_or("None") + ";",
                                       "\tlevel.waw2bo2_source_player_laststand = ::" + laststand.value_or("None") + ";"};
        if (const auto init = ExtractEntry(LASTSTAND, "init"))
            lines.push_back("\t" + *init + "();");
        lines.push_back(std::string("\t") + FIXES + "::wrap_player_damage();");
        const std::string name = "waw2bo2_source_player_damage";
        levelStateParts.push_back("\n// WaW drives player damage and last stand: the map's engine entries\n" + name + "()\n{\n" + Join(lines, "\n") + "\n}\n");
        report.rewrites["player damage and last stand -> source (WaW drives)"] += 1;
        return name;
    }

    std::optional<std::string> Translator::ExtractRoundHealth(const std::string& coreIn, const std::string& functionIn, const bool sourceLoop)
    {
        const std::string core = coreIn.empty() ? ZOMBIEMODE : coreIn;
        const std::string function = functionIn.empty() ? ROUND_SPAWNING : functionIn;
        const auto script = sources.Get(core);
        const Function* fnp = script ? script->Find(function) : nullptr;
        if (!fnp)
            return std::nullopt;
        const Function fn = *fnp;
        const auto& toks = script->tokens;
        const auto where = Origin(sources, core + ".gsc", core);
        const auto touchesHealth = [&](const Function& callee)
        {
            const auto body = Lows(toks, static_cast<long long>(callee.bodyOpen), static_cast<long long>(callee.bodyClose));
            for (size_t i = 0; i + 2 < body.size(); i++)
                if (body[i] == "level" && body[i + 1] == "." && body[i + 2] == "zombie_health")
                    return true;
            return false;
        };
        std::optional<std::string> step;
        for (const auto& [start, end] : TopLevelStatements(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
        {
            const Function* callee = At(toks, start).kind == Kind::Ident ? script->Find(At(toks, start).Low()) : nullptr;
            std::vector<std::string> words;
            for (const auto& t : SliceCopy(toks, start, end))
                words.push_back(t.text);
            if (!callee || words.size() != 4 || words[1] != "(" || words[2] != ")" || words[3] != ";")
                continue;
            if (touchesHealth(*callee))
            {
                step = At(toks, start).Low();
                break;
            }
        }
        const Function* loop = script->Find("round_think");
        if (!step && sourceLoop && loop)
        {
            const auto loopToks = SliceCopy(toks, static_cast<long long>(loop->bodyOpen) + 1, static_cast<long long>(loop->bodyClose));
            for (long long i = 1; i < static_cast<long long>(loopToks.size()) - 3; i++)
            {
                const auto& t = loopToks[static_cast<size_t>(i)];
                const Function* callee = t.kind == Kind::Ident ? script->Find(t.Low()) : nullptr;
                if (!callee || loopToks[static_cast<size_t>(i + 1)].text != "(" || loopToks[static_cast<size_t>(i + 2)].text != ")"
                    || loopToks[static_cast<size_t>(i + 3)].text != ";" || !In(loopToks[static_cast<size_t>(i - 1)].text, {";", "{", "}"}))
                    continue;
                if (touchesHealth(*callee))
                {
                    report.rewrites["WaW health step " + t.Low() + " in the source round loop"] += 1;
                    levelStateParts.push_back("\n// " + core + "::round_think calls " + t.Low() + " itself (the source loop runs it)\n"
                                              "waw2bo2_health_by_round_loop()\n{\n}\n\n"
                                              "waw2bo2_health_in_round_loop_register()\n{\n"
                                              "\tlevel.waw2bo2_health_in_round_loop = 1;\n"
                                              "\tlevel.waw2bo2_round_health_func = ::waw2bo2_health_by_round_loop;\n}\n");
                    return std::string("waw2bo2_health_in_round_loop_register");
                }
            }
        }
        if (!step)
        {
            report.Unsupported("GSC " + core + "::" + function + " health step (no zombie health function called; BO2's runs)", where);
            return std::nullopt;
        }
        const auto extracted = ExtractEntry(core, *step);
        const auto name = Prefix(core) + "__" + *step + "_register";
        levelStateParts.push_back("\n// " + core + "::" + function + " calls " + *step + " once per round it runs (never on a dog round)\n" + name
                                  + "()\n{\n\tlevel.waw2bo2_round_health_func = ::" + extracted.value_or("None") + ";\n}\n");
        return name;
    }

    std::optional<std::string> Translator::ExtractRoundCounter(const std::string& coreIn, const bool sourceLoop)
    {
        const std::string core = coreIn.empty() ? ZOMBIEMODE : coreIn;
        const auto& contract = Api()["stock_map_policies"]["waw_round_counter"];
        const auto script = sources.Get(core);
        const auto where = Origin(sources, core + ".gsc", core);
        const std::string roundStart = contract["round_start"], roundEnd = contract["round_end"];
        const std::vector<std::string> names{roundStart, roundEnd};
        if (!script || !script->Find(roundStart) || !script->Find(roundEnd))
        {
            report.Unsupported(FALLBACK, where + ": no WaW round display (" + roundStart + ", " + roundEnd + "); BO2's counter stays");
            return std::nullopt;
        }
        const auto& toks = script->tokens;
        std::set<std::string> read;
        for (const auto& n : names)
        {
            const Function& fn = *script->Find(n);
            const auto f = LevelFieldsRead(toks, static_cast<long long>(fn.bodyOpen) + 2, static_cast<long long>(fn.bodyClose));
            read.insert(f.begin(), f.end());
        }
        std::set<std::string> created;
        const Function* init = script->Find(contract["state_init"].get<std::string>());
        if (init)
            for (const auto& [start, end] : TopLevelStatements(toks, static_cast<long long>(init->bodyOpen), static_cast<long long>(init->bodyClose)))
            {
                const auto words = SliceCopy(toks, start, end);
                if (words.size() > 5 && words[0].Low() == "level" && words[1].text == "." && words[3].text == "=" && words[4].kind == Kind::Ident
                    && script->Find(words[4].Low()))
                    created.insert(words[2].Low());
            }
        std::set<std::string> readCreated;
        for (const auto& f : read)
            if (created.count(f))
                readCreated.insert(f);
        std::set<std::string> helpers;
        std::vector<const Function*> queueFns{script->Find(roundStart), script->Find(roundEnd)};
        if (init)
            for (const auto& [start, end] : TopLevelStatements(toks, static_cast<long long>(init->bodyOpen), static_cast<long long>(init->bodyClose)))
                if (At(toks, start).Low() == "level" && readCreated.count(At(toks, start + 2).Low()))
                    for (const auto& r : References(toks, static_cast<size_t>(start), static_cast<size_t>(end)))
                        if (!r.qualifier && script->Find(Lower(r.name)))
                        {
                            helpers.insert(Lower(r.name));
                            queueFns.push_back(script->Find(Lower(r.name)));
                        }
        while (!queueFns.empty())
        {
            const Function* fn = queueFns.back();
            queueFns.pop_back();
            for (const auto& r : References(toks, fn->bodyOpen, fn->bodyClose))
            {
                const auto name = Lower(r.name);
                if (!r.qualifier && script->Find(name) && !helpers.count(name) && std::find(names.begin(), names.end(), name) == names.end())
                {
                    helpers.insert(name);
                    queueFns.push_back(script->Find(name));
                }
            }
        }
        const auto mapped = ApiString(Api()["core_scripts"], core);
        const bool driven = ApiSection("source_driven").contains(core);
        for (const auto& name : helpers)
            if (driven || !(mapped && !mapped->empty() && api.Defines(*mapped, name)))
                ExtractEntry(core, name);
        const auto state = readCreated.empty() ? std::nullopt : ExtractLevelState(core, contract["state_init"].get<std::string>(), readCreated);
        const auto start = ExtractEntry(core, roundStart);
        const auto end = ExtractEntry(core, roundEnd);
        std::set<std::string> literals;
        for (const auto& n : names)
        {
            const Function& fn = *script->Find(n);
            for (const auto& t : SliceCopy(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
                if (t.kind == Kind::String)
                    literals.insert(StripChars(t.text, "\""));
        }
        std::vector<std::string> precache;
        for (const auto& fnName : contract["precache_functions"])
        {
            const Function* fn = script->Find(fnName.get<std::string>());
            if (!fn)
                continue;
            for (const auto& ref : References(toks, fn->bodyOpen, fn->bodyClose))
            {
                if (Lower(ref.name) != "precacheshader" || At(toks, static_cast<long long>(ref.index) + 2).kind != Kind::String)
                    continue;
                const auto material = StripChars(At(toks, static_cast<long long>(ref.index) + 2).text, "\"");
                bool prefixed = false;
                for (const auto& l : literals)
                    prefixed = prefixed || (!l.empty() && StartsWith(material, l));
                if (prefixed && std::find(precache.begin(), precache.end(), material) == precache.end())
                    precache.push_back(material);
            }
        }
        for (const auto& m : precache)
            if (std::find(report.wawHudMaterials.begin(), report.wawHudMaterials.end(), m) == report.wawHudMaterials.end())
                report.wawHudMaterials.push_back(m);
        std::vector<std::string> lines;
        if (sourceLoop)
        {
            lines = {"", "// " + core + ": the source round display's HUD state (the source loop draws it)", "waw2bo2_waw_round_counter()", "{",
                     "\tlevel.waw2bo2_waw_round_counter = 1;"};
            for (const auto& m : precache)
                lines.push_back("\tprecacheshader( \"" + m + "\" );");
            if (state)
                lines.push_back("\t" + *state + "();");
            lines.insert(lines.end(), {"}", ""});
            levelStateParts.push_back(Join(lines, "\n"));
            report.rewrites["source round display (source loop)"] += 1;
            return std::string("waw2bo2_waw_round_counter");
        }
        lines = {"", "// " + core + ": WaW's round counter (stock_map_policies waw_round_counter); BO2's 8-bit", "// setroundsplayed display and its 255 round clamp stay unused",
                 "waw2bo2_waw_round_counter()", "{", "\tlevel.waw2bo2_waw_round_counter = 1;"};
        for (const auto& m : precache)
            lines.push_back("\tprecacheshader( \"" + m + "\" );");
        lines.insert(lines.end(), {"\tlevel.zombie_round_change_custom = ::waw2bo2_waw_round_start;", std::string("\tlevel.round_think_func = ") + OWNED_ZM + "::round_think;",
                                   "\tlevel thread waw2bo2_waw_round_end();", "}", "", "waw2bo2_waw_round_start()", "{",
                                   "\tif ( !isdefined( level.waw2bo2_waw_round_hud ) )", "\t{", "\t\tlevel.waw2bo2_waw_round_hud = 1;"});
        if (state)
            lines.push_back("\t\t" + *state + "();");
        lines.insert(lines.end(), {"\t}", "\tlevel thread maps\\mp\\zombies\\_zm_audio::change_zombie_music( \"round_start\" );", "\t" + start.value_or("None") + "();", "}", "",
                                   "waw2bo2_waw_round_end()", "{", "\tlevel endon( \"end_game\" );", "\tfor ( ;; )", "\t{", "\t\tlevel waittill( \"end_of_round\" );",
                                   "\t\tlevel thread " + end.value_or("None") + "();", "\t}", "}", ""});
        levelStateParts.push_back(Join(lines, "\n"));
        report.rewrites["stock WaW map round counter (source display)"] += 1;
        return std::string("waw2bo2_waw_round_counter");
    }

    std::optional<std::vector<std::string>> Translator::RoundLoopMissingAnchors(const std::string& coreIn, const std::string& function)
    {
        const auto script = sources.Get(coreIn.empty() ? ZOMBIEMODE : coreIn);
        const Function* fn = script ? script->Find(function) : nullptr;
        if (!fn)
            return std::nullopt;
        const auto body = EmitRange(script->tokens, static_cast<long long>(fn->bodyOpen), static_cast<long long>(fn->bodyClose) + 1);
        std::vector<std::string> missing;
        for (const auto& [label, pattern] : Anchors())
        {
            const pyre::Regex re(Replace(pattern, "ROUND_WAIT", "round_wait"), pyre::IGNORECASE);
            if (re.FindIter(body).size() != 1)
                missing.push_back(label);
        }
        return missing;
    }

    std::optional<std::string> Translator::ExtractRoundLoop(const std::string& coreIn, const std::string& function)
    {
        const std::string core = coreIn.empty() ? ZOMBIEMODE : coreIn;
        const auto script = sources.Get(core);
        const Function* fn = script ? script->Find(function) : nullptr;
        if (!fn)
        {
            if (RoundStartDrives(core))
                return ExtractRoundStartDriver(core, Origin(sources, core + ".gsc", core), {"no " + function});
            return std::nullopt;
        }
        const auto where = Origin(sources, core + ".gsc", core);
        const auto missing = RoundLoopMissingAnchors(core, function);
        if (missing && !missing->empty() && RoundStartDrives(core))
            return ExtractRoundStartDriver(core, where, *missing);
        if (missing && !missing->empty())
        {
            std::string list;
            for (const auto& m : *missing)
                list += (list.empty() ? "'" : ", '") + m + "'";
            report.Unsupported("GSC " + core + "::" + function + " round loop (no anchors [" + list + "]; BO2's loop runs)", where);
            return std::nullopt;
        }
        const auto name = ExtractEntry(core, function);
        auto* self = this;
        postPatches[{core, function}] = [self, where, function](std::string text)
        {
            for (const auto& [label, pattern] : Anchors())
            {
                const pyre::Regex re(Replace(pattern, "ROUND_WAIT", R"(\w*round_wait)"), pyre::IGNORECASE);
                const auto [before, after] = GlueCalls(label);
                size_t count = 0;
                text = re.Sub(
                    [&, label = label, before = before, after = after](const pyre::Match& m)
                    {
                        const std::string mirror =
                            label == "advance" && Lower(m[1]) != "round_number" ? "\n\t\tlevel.round_number = level." + m[1] + ";" : std::string();
                        return (before.empty() ? std::string() : std::string(OWNED_ZM) + "::" + before + "();\n\t\t") + m[0] + mirror
                               + (after.empty() ? std::string() : std::string("\n\t\t") + OWNED_ZM + "::" + after + "();");
                    },
                    text, 1, &count);
                if (count != 1)
                    throw std::runtime_error(where + ": " + function + " translated without anchor " + label);
            }
            static const pyre::Regex local(R"((?<![.\w])round_spawn_func\s*=\s*[^;]+;)");
            size_t made = 0;
            text = local.Sub("round_spawn_func = level.round_spawn_func;", text, 0, &made);
            if (made)
                self->report.rewrites["source loop's local spawn function -> level.round_spawn_func"] += static_cast<long long>(made);
            return text;
        };
        std::optional<std::string> spawning;
        if (script->Find("round_spawning") && SourceDrives(core, "round_spawning"))
            spawning = ExtractEntry(core, "round_spawning");
        if (spawning)
            report.rewrites["WaW drives the spawning: source round_spawning -> round_spawn_func"] += 1;
        const std::string spawnLines =
            spawning ? "\tlevel.waw2bo2_source_round_spawning = ::" + *spawning + ";\n\tlevel.round_spawn_func = ::" + *spawning + ";\n" : std::string();
        report.rewrites["WaW drives the rounds: source round_think -> round_think_func"] += 1;
        report.extracted.push_back(core + "::" + function + " (round loop -> round_think_func)");
        levelStateParts.push_back("\n// " + core + "::" + function + ": the map's round loop drives the rounds; BO2's glue runs at its anchors\n"
                                  "waw2bo2_source_round_think( restart )\n{\n\tlevel endon( \"end_round_think\" );\n\t"
                                  + std::string(OWNED_ZM) + "::waw2bo2_round_think_begin( restart );\n\t" + name.value_or("None")
                                  + "();\n}\n\nwaw2bo2_source_round_loop()\n{\n\tlevel.waw2bo2_source_round_loop = 1;\n" + spawnLines
                                  + "\tlevel.round_think_func = ::waw2bo2_source_round_think;\n}\n");
        return std::string("waw2bo2_source_round_loop");
    }

    bool Translator::RoundStartDrives(const std::string& coreIn)
    {
        const auto script = sources.Get(coreIn.empty() ? ZOMBIEMODE : coreIn);
        const Function* fn = script ? script->Find("round_start") : nullptr;
        if (!fn)
            return false;
        for (const auto& r : References(script->tokens, fn->bodyOpen + 1, fn->bodyClose))
            if (Lower(r.name) == "round_think")
                return false;
        return true;
    }

    std::string Translator::ExtractRoundStartDriver(const std::string& core, const std::string& where, const std::vector<std::string>& missing)
    {
        const auto name = ExtractEntry(core, "round_start");
        report.rewrites["WaW drives the rounds: source round_start -> round_think_func"] += 1;
        report.extracted.push_back(core + "::round_start (round driver -> round_think_func)");
        std::string list;
        for (const auto& m : missing)
            list += (list.empty() ? "'" : ", '") + m + "'";
        report.Unsupported("GSC " + core + "::round_think anchors [" + list
                               + "]: the map's round_start runs its own round system; BO2's per-round glue (round signals, stats) does not run",
                           where);
        levelStateParts.push_back("\n// " + core + "::round_start: the map's own round system drives the rounds\n"
                                  "waw2bo2_source_round_think( restart )\n{\n\tlevel endon( \"end_round_think\" );\n\t"
                                  + std::string(OWNED_ZM)
                                  + "::waw2bo2_round_think_begin( restart );\n\tif ( !isdefined( level.waw2bo2_round_driver_started ) )\n\t{\n\t\tlevel.waw2bo2_round_driver_started = 1;\n\t\t"
                                  + name.value_or("None")
                                  + "();\n\t}\n\tlevel waittill( \"end_game\" );\n}\n\nwaw2bo2_source_round_loop()\n{\n\tlevel.waw2bo2_source_round_loop = 1;\n\tlevel.round_think_func = ::waw2bo2_source_round_think;\n}\n");
        return "waw2bo2_source_round_loop";
    }

    const Function* Translator::MapFrameworkFunction(const std::string& name)
    {
        const auto key = std::string(ZOMBIEMODE) + ".gsc";
        const auto variants = sources.StockVariants(ZOMBIEMODE);
        const auto* own = sources.text->Find(key);
        if (!own || variants.empty() || std::find(variants.begin(), variants.end(), *own) != variants.end())
            return nullptr;
        const auto script = sources.Get(ZOMBIEMODE);
        const Function* fn = script ? script->Find(Lower(name)) : nullptr;
        if (!fn)
            return nullptr;
        const auto body = Lows(script->tokens, static_cast<long long>(fn->start), static_cast<long long>(fn->bodyClose) + 1);
        for (const auto& variant : variants)
        {
            const auto stock = Parse(variant, ZOMBIEMODE);
            const Function* base = stock.Find(Lower(name));
            if (base && Lows(stock.tokens, static_cast<long long>(base->start), static_cast<long long>(base->bodyClose) + 1) == body)
                return nullptr;
        }
        return fn;
    }

    std::optional<std::string> Translator::ExtractCustomIntermission()
    {
        const auto script = sources.Get(ZOMBIEMODE);
        if (!script)
            return std::nullopt;
        const auto& toks = script->tokens;
        std::optional<std::string> target;
        for (long long i = 0; i < static_cast<long long>(toks.size()) - 5; i++)
            if (WordsEqual(toks, i, {"level", ".", "custom_intermission", "=", "::"}))
            {
                target = At(toks, i + 5).Low();
                break;
            }
        if (!target || !MapFrameworkFunction(*target))
            return std::nullopt;
        intermissionFuncs.insert({ZOMBIEMODE, *target});
        const auto name = ExtractEntry(ZOMBIEMODE, *target);
        if (!name)
            return std::nullopt;
        postCalls.push_back("level.custom_intermission = " + std::string(CORE) + "::" + *name + ";");
        report.rewrites["map custom_intermission -> set after _zm::init"] += 1;
        return name;
    }

    std::vector<std::string> Translator::ExtractEndGame()
    {
        const Function* fnp = MapFrameworkFunction("end_game");
        if (!fnp)
            return {};
        const Function fn = *fnp;
        const auto script = sources.Get(ZOMBIEMODE);
        const auto& toks = script->tokens;
        const auto where = Origin(sources, std::string(ZOMBIEMODE) + ".gsc", ZOMBIEMODE);
        const auto prefix = Prefix(ZOMBIEMODE);
        const auto find = [](const Tokens& ts, const std::vector<Span>& spans, const std::vector<std::string>& words, const size_t after = 0) -> std::optional<size_t>
        {
            for (size_t k = 0; k < spans.size(); k++)
                if (k >= after && Lows(ts, spans[k].first, spans[k].second) == words)
                    return k;
            return std::nullopt;
        };
        const auto intermissionWait = [&](const Tokens& ts, const std::vector<Span>& spans) -> std::optional<std::pair<size_t, size_t>>
        {
            const auto begin = find(ts, spans, {"intermission", "(", ")", ";"});
            const auto stop = find(ts, spans, {"level", "notify", "(", "\"stop_intermission\"", ")", ";"}, begin.value_or(0));
            if (!begin || !stop)
                return std::nullopt;
            return std::make_pair(*begin + 1, *stop);
        };
        const auto make = [&](const std::string& name, const std::string& comment, const Tokens& body, const std::string& tail = "")
        {
            Tokens tokens{Tok(Kind::Ident, name, std::string("\n// ") + ZOMBIEMODE + "::end_game: " + comment + "\n"), Tok(Kind::Punct, "("), Tok(Kind::Punct, ")"),
                          Tok(Kind::Punct, "{", "\n")};
            tokens.insert(tokens.end(), body.begin(), body.end());
            tokens.push_back(Tok(Kind::Punct, "}", "\n"));
            tokens.push_back(Tok(Kind::Eof, ""));
            report.rewrites["syntax fixes (T6 compiler)"] += FixSyntax(tokens);
            report.rewrites["WaW-owned level fields renamed (level.waw_*)"] += RenameLevelFields(tokens);
            ResolveRefs(tokens, *script, where, CoreIncludes(), {0}, std::string(ZOMBIEMODE));
            At(tokens, -2).text = tail + At(tokens, -2).text;
            levelStateParts.push_back(Emit(tokens));
            return name;
        };
        const auto spans = TopLevelStatements(toks, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose));
        std::vector<std::string> names;
        const auto wait = find(toks, spans, {"level", "waittill", "(", "\"end_game\"", ")", ";"});
        if (wait && *wait)
        {
            names.push_back(make(prefix + "__end_game_prelude", "the map's statements before the end_game wait", SliceCopy(toks, spans[0].first, spans[*wait - 1].second)));
            report.extracted.push_back(std::string(ZOMBIEMODE) + "::end_game (map prelude)");
        }
        const auto span = intermissionWait(toks, spans);
        if (span && span->first < span->second)
        {
            const auto body = Lows(toks, spans[span->first].first, spans[span->second - 1].second);
            std::vector<std::vector<std::string>> stock;
            for (const auto& variant : sources.StockVariants(ZOMBIEMODE))
            {
                const auto parsed = Parse(variant, ZOMBIEMODE);
                const Function* base = parsed.Find("end_game");
                if (!base)
                    continue;
                const auto baseSpans = TopLevelStatements(parsed.tokens, static_cast<long long>(base->bodyOpen), static_cast<long long>(base->bodyClose));
                const auto found = intermissionWait(parsed.tokens, baseSpans);
                if (found && found->first < found->second)
                    stock.push_back(Lows(parsed.tokens, baseSpans[found->first].first, baseSpans[found->second - 1].second));
            }
            if (std::find(stock.begin(), stock.end(), body) == stock.end())
            {
                auto head = Tokenize("\n\tlevel waittill( \"end_game\" );"
                                     "\n\t// BO2's end_game waits zombie_intermission_time after intermission(); this map's own"
                                     "\n\t// wait decides instead. BO2's timer only ends a cutscene that never finishes."
                                     "\n\tlevel.zombie_vars[\"zombie_intermission_time\"] = "
                                     + std::to_string(END_GAME_FALLBACK) + ";\n\tlevel waittill( \"intermission\" );");
                head.pop_back();
                for (auto& t : head)
                    t.line = 0;
                const auto rest = SliceCopy(toks, spans[span->first].first, spans[span->second - 1].second);
                head.insert(head.end(), rest.begin(), rest.end());
                const std::string tail = "\tlevel notify( \"stop_intermission\" );"
                                         "\n\tarray_thread( get_players(), maps\\mp\\zombies\\_zm::player_exit_level );"
                                         "\n\twait 1.5;\n\tplayers = get_players();"
                                         "\n\tfor ( i = 0; i < players.size; i++ )\n\t\tplayers[i] cameraactivate( 0 );"
                                         "\n\texitlevel( 0 );\n";
                names.push_back(make(prefix + "__end_game_intermission_wait", "the map's wait after intermission(), then the exit", head, tail));
                report.extracted.push_back(std::string(ZOMBIEMODE) + "::end_game (intermission wait)");
                report.rewrites["map end_game intermission wait -> replaces BO2's fixed wait"] += 1;
            }
        }
        for (const auto& name : names)
            postCalls.push_back("level thread " + std::string(CORE) + "::" + name + "();");
        return names;
    }
} // namespace w2b::gsc::port

// Port of zombiebehavior.py (see zombiebehavior.h).
#include "zombiebehavior.h"

#include "api.h"
#include "core/files.h"
#include "core/pyre.h"
#include "core/unicode.h"
#include "statements.h"
#include "translator.h"

#include <algorithm>
#include <stdexcept>

namespace w2b::gsc::port::zombiebehavior
{
    namespace fs = std::filesystem;

    namespace
    {
        using Words = std::vector<std::string>;

        // [t.low for t in tokens[a:a + n]]
        Words Lows(const Tokens& tokens, const long long a, const long long n)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, a + n);
            Words words;
            for (size_t i = lo; i < hi; i++)
                words.push_back(tokens[i].Low());
            return words;
        }

        Words LowsRange(const Tokens& tokens, const long long a, const long long b)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            Words words;
            for (size_t i = lo; i < hi; i++)
                words.push_back(tokens[i].Low());
            return words;
        }

        // words[a:b] == expected
        bool SliceIs(const Words& words, const long long a, const long long b, const Words& expected)
        {
            const auto [lo, hi] = Slice(words.size(), a, b);
            if (hi - lo != expected.size())
                return false;
            for (size_t i = 0; i < expected.size(); i++)
                if (words[lo + i] != expected[i])
                    return false;
            return true;
        }

        const std::string& Word(const Words& words, const size_t i)
        {
            if (i >= words.size())
                throw std::out_of_range("list index out of range");
            return words[i];
        }

        std::string S(const py::Json& value)
        {
            return value.get<std::string>();
        }

        Tokens Copy(const Tokens& tokens, const long long a, const long long b)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            return Tokens(tokens.begin() + static_cast<long long>(lo), tokens.begin() + static_cast<long long>(hi));
        }

        Token Punct(const std::string& text)
        {
            Token t;
            t.kind = Kind::Punct;
            t.text = text;
            return t;
        }

        // byte offset of code point offset cp in UTF-8 text
        size_t ByteOffset(const std::string& text, const long long cp)
        {
            size_t i = 0;
            for (long long n = 0; n < cp && i < text.size(); n++)
            {
                const auto c = static_cast<unsigned char>(text[i]);
                i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            }
            return i;
        }

        std::vector<std::string> SplitWhitespace(const std::string& line)
        {
            std::vector<std::string> words;
            std::string word;
            for (const char c : line)
            {
                if (py::IsSpace(c))
                {
                    if (!word.empty())
                        words.push_back(word);
                    word.clear();
                }
                else
                    word += c;
            }
            if (!word.empty())
                words.push_back(word);
            return words;
        }

        std::vector<std::string> SplitOn(const std::string& text, const char sep)
        {
            std::vector<std::string> parts{""};
            for (const char c : text)
            {
                if (c == sep)
                    parts.emplace_back();
                else
                    parts.back() += c;
            }
            return parts;
        }

        size_t Count(const std::string& text, const std::string& needle)
        {
            size_t n = 0;
            for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
                n++;
            return n;
        }

        std::string RStripNewlines(std::string text)
        {
            while (!text.empty() && text.back() == '\n')
                text.pop_back();
            return text;
        }

        bool IsDigits(const std::string& text)
        {
            if (text.empty())
                return false;
            for (const char c : text)
                if (!py::IsDigit(c))
                    return false;
            return true;
        }

        long long IntOfFloat(const std::string& text)
        {
            double value = 0;
            if (!py::ParseFloat(text, value))
                throw std::invalid_argument("could not convert string to float: " + Repr(text));
            return static_cast<long long>(value);
        }

        const Function* FunctionOf(const ScriptPtr& script, const std::string& name)
        {
            return script ? script->Find(name) : nullptr;
        }

        // _rise_result: the local receiving the spawn-completion event payload
        std::optional<std::string> RiseResult(const Script& script, const Function& fn, const std::string& event)
        {
            std::set<std::string> results;
            const auto& toks = script.tokens;
            for (long long i = static_cast<long long>(fn.bodyOpen) + 1; i < static_cast<long long>(fn.bodyClose) - 5; i++)
            {
                const auto words = Lows(toks, i, 3);
                if (SliceIs(words, 0, 3, {"self", "waittill", "("}) && At(toks, i + 3).Low() == "\"" + Lower(event) + "\"" && At(toks, i + 4).text == ","
                    && At(toks, i + 5).kind == Kind::Ident)
                    results.insert(At(toks, i + 5).text);
            }
            if (results.size() > 1)
                throw std::runtime_error(script.path + "::" + fn.name + ": ambiguous " + event + " event result");
            if (results.empty())
                return std::nullopt;
            return *results.begin();
        }

        // _level_fields_assigned
        std::set<std::string> LevelFieldsAssignedReachable(Sources& sources, const std::string& mapName)
        {
            std::vector<std::string> queue{"maps\\" + mapName};
            std::set<std::string> seen, fields;
            while (!queue.empty())
            {
                auto path = Replace(Lower(queue.back()), "/", "\\");
                queue.pop_back();
                if (seen.count(path))
                    continue;
                seen.insert(path);
                const auto script = sources.Get(path);
                if (!script)
                    continue;
                queue.insert(queue.end(), script->includes.begin(), script->includes.end());
                for (const auto& ref : References(script->tokens))
                    if (ref.qualifier)
                        queue.push_back(*ref.qualifier);
                const auto& tokens = script->tokens;
                for (long long i = 0; i < static_cast<long long>(tokens.size()) - 3; i++)
                    if (tokens[i].Low() == "level" && tokens[i + 1].text == "." && tokens[i + 2].kind == Kind::Ident && In(tokens[i + 3].text, {"=", "["})
                        && (i == 0 || tokens[i - 1].text != "."))
                        fields.insert(tokens[i + 2].Low());
            }
            return fields;
        }

        struct NoGuard
        {
        };

        // _folded_body
        Tokens FoldedBody(Sources& sources, const Script& script, const Function& fn, const std::optional<std::string>& mapName)
        {
            std::optional<std::set<std::string>> assigned;
            if (mapName && !mapName->empty())
                assigned = LevelFieldsAssignedReachable(sources, *mapName);
            const auto& toks = script.tokens;
            std::function<Tokens(long long, long long)> fold = [&](const long long opening, const long long closing)
            {
                Tokens result;
                for (const auto& [a, b] : TopLevelStatements(toks, opening, closing))
                {
                    const auto original = Copy(toks, a, b);
                    if (!mapName || At(toks, a).Low() != "if")
                    {
                        result.insert(result.end(), original.begin(), original.end());
                        continue;
                    }
                    const auto guardEnd = static_cast<long long>(Match(toks, static_cast<size_t>(a + 1), "(", ")"));
                    const auto words = LowsRange(toks, a + 2, guardEnd);
                    const auto matched = FoldLevelGuard(words, *mapName, assigned ? &*assigned : nullptr);
                    if (!matched)
                    {
                        result.insert(result.end(), original.begin(), original.end());
                        continue;
                    }
                    const auto yes = guardEnd + 1;
                    if (At(toks, yes).text != "{")
                    {
                        result.insert(result.end(), original.begin(), original.end());
                        continue;
                    }
                    const auto yesEnd = static_cast<long long>(Match(toks, static_cast<size_t>(yes), "{", "}"));
                    std::optional<long long> no, noEnd;
                    if (yesEnd + 1 < b && At(toks, yesEnd + 1).Low() == "else")
                    {
                        no = yesEnd + 2;
                        if (At(toks, *no).text != "{")
                        {
                            result.insert(result.end(), original.begin(), original.end());
                            continue;
                        }
                        noEnd = static_cast<long long>(Match(toks, static_cast<size_t>(*no), "{", "}"));
                    }
                    if (*matched)
                    {
                        const auto inner = fold(yes, yesEnd);
                        result.insert(result.end(), inner.begin(), inner.end());
                    }
                    else if (no)
                    {
                        const auto inner = fold(*no, *noEnd);
                        result.insert(result.end(), inner.begin(), inner.end());
                    }
                }
                return result;
            };
            Tokens out{toks[fn.bodyOpen]};
            const auto inner = fold(static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose));
            out.insert(out.end(), inner.begin(), inner.end());
            out.push_back(toks[fn.bodyClose]);
            return out;
        }

        std::string ReplaceState(const std::string& asd, const std::string& state, const std::vector<std::string>& assets)
        {
            const pyre::Regex pattern("(^" + pyre::Escape(state) + R"(\s*:[^\n]*\n\{).*?^\})", pyre::MULTILINE | pyre::DOTALL);
            size_t count = 0;
            const auto out = pattern.Sub(
                [&](const pyre::Match& m)
                {
                    std::string body = m[1] + "\n";
                    for (const auto& a : assets)
                        body += "\t" + a + "\n";
                    return body + "}";
                },
                asd, 0, &count);
            if (count != 1)
                throw std::runtime_error("Native animation state " + state + ": expected one definition");
            return out;
        }

        std::string PyStripRepr40(const std::string& anchor)
        {
            const auto stripped = PyStrip(anchor);
            return Repr(stripped.substr(0, std::min<size_t>(40, stripped.size())));
        }
    } // namespace

    std::string Format(const std::string_view text, const std::map<std::string, std::string>& values)
    {
        std::string out;
        out.reserve(text.size() * 2);
        for (size_t i = 0; i < text.size(); i++)
        {
            const char c = text[i];
            if (c == '{' && i + 1 < text.size() && text[i + 1] == '{')
            {
                out += '{';
                i++;
            }
            else if (c == '}' && i + 1 < text.size() && text[i + 1] == '}')
            {
                out += '}';
                i++;
            }
            else if (c == '{')
            {
                const auto close = text.find('}', i);
                const std::string key(text.substr(i + 1, close - i - 1));
                const auto found = values.find(key);
                if (found == values.end())
                    throw std::logic_error("template key missing: " + key);
                out += found->second;
                i = close;
            }
            else
                out += c;
        }
        return out;
    }

    std::optional<std::string> SourceChasePolicy(Sources& sources, const py::Json& definition)
    {
        const auto& contract = definition["source_chase"];
        const auto source = sources.Get(S(contract["script"]));
        const Function* fnp = FunctionOf(source, S(contract["function"]));
        if (!fnp)
            return std::nullopt;
        const Function fn = *fnp;
        const auto& tokens = source->tokens;
        for (const auto& [a, b] : TopLevelStatements(tokens, static_cast<long long>(fn.bodyOpen), static_cast<long long>(fn.bodyClose)))
        {
            if (At(tokens, a).Low() != "if")
                continue;
            std::vector<std::string> branches;
            long long i = a;
            bool found = false;
            while (i < b)
            {
                const auto headerStart = i;
                if (At(tokens, i).Low() == "else")
                    i++;
                if (At(tokens, i).Low() == "if")
                {
                    if (At(tokens, i + 1).text != "(")
                        break;
                    i = static_cast<long long>(Match(tokens, static_cast<size_t>(i + 1), "(", ")")) + 1;
                }
                if (At(tokens, i).text != "{")
                    break;
                const auto close = static_cast<long long>(Match(tokens, static_cast<size_t>(i), "{", "}"));
                const auto refs = References(tokens, static_cast<size_t>(i + 1), static_cast<size_t>(close));
                bool chaseCall = false, setupCall = false;
                for (const auto& r : refs)
                {
                    chaseCall = chaseCall || Lower(r.name) == S(contract["chase_function"]);
                    setupCall = setupCall || Lower(r.name) == S(contract["setup_function"]);
                }
                const bool chase = chaseCall && setupCall;
                found = found || chase;
                branches.push_back(EmitRange(tokens, headerStart, i) + "{ return " + (chase ? "true" : "false") + "; }");
                i = close + 1;
                if (i >= b || At(tokens, i).Low() != "else")
                    break;
            }
            if (found)
            {
                const auto result = RiseResult(*source, fn, S(contract["rise_event"])).value_or("rise_result");
                std::vector<std::string> lines{"// Source zombie_think chase/entrance decision.", "should_chase( " + result + " )", "{"};
                lines.insert(lines.end(), branches.begin(), branches.end());
                lines.push_back("return false;");
                lines.push_back("}");
                return Join(lines, "\n");
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> SourceSpeedState(Sources& sources, const py::Json& definition, const bool rounds)
    {
        const auto& contract = definition["source_speed"];
        const auto& roundContract = definition["source_round_state"];
        const auto owner = [&](const Script& script, const Ref& ref) -> const Script*
        {
            // Sources keeps every parsed script alive: plain pointers stay valid
            if (ref.qualifier)
                return sources.Get(*ref.qualifier).get();
            if (script.Find(Lower(ref.name)))
                return &script;
            for (const auto& path : script.includes)
                if (const auto dep = sources.Get(path); dep && dep->Find(Lower(ref.name)))
                    return dep.get();
            return nullptr;
        };
        using Fields = std::pair<std::set<std::string>, std::set<std::string>>;
        std::function<Fields(const Script&, const std::string&, std::set<std::pair<std::string, std::string>>&)> fields;
        fields = [&](const Script& script, const std::string& name, std::set<std::pair<std::string, std::string>>& seen) -> Fields
        {
            const std::pair<std::string, std::string> key{script.path, name};
            const Function* fn = script.Find(name);
            if (seen.count(key) || !fn)
                return {};
            seen.insert(key);
            const auto& tokens = script.tokens;
            Fields rw;
            for (long long i = static_cast<long long>(fn->bodyOpen) + 1; i < static_cast<long long>(fn->bodyClose) - 3; i++)
            {
                if (SliceIs(Lows(tokens, i, 2), 0, 2, {"level", "."}))
                {
                    const auto field = At(tokens, i + 2).Low();
                    if (In(At(tokens, i + 3).text, {"=", "+=", "-=", "++", "--"}))
                        rw.second.insert(field);
                    if (At(tokens, i + 3).text != "=")
                        rw.first.insert(field);
                }
            }
            for (const auto& ref : References(tokens, fn->bodyOpen + 1, fn->bodyClose))
                if (const auto dep = owner(script, ref))
                {
                    const auto [r, w] = fields(*dep, Lower(ref.name), seen);
                    rw.first.insert(r.begin(), r.end());
                    rw.second.insert(w.begin(), w.end());
                }
            return rw;
        };
        const auto writes = [&](const Script& script, const std::string& name)
        {
            std::set<std::pair<std::string, std::string>> seen;
            return fields(script, name, seen).second;
        };
        const auto speed = sources.Get(S(contract["script"]));
        if (!speed || !speed->Find(S(contract["function"])))
            return std::nullopt;
        std::set<std::string> reads;
        {
            std::set<std::pair<std::string, std::string>> seen;
            reads = fields(*speed, S(contract["function"]), seen).first;
        }
        reads.erase("round_number");
        reads.erase("zombie_vars");
        std::vector<std::string> initial, start, end;
        if (const Function* initializer = speed->Find("init"))
            for (const auto& [a, b] : TopLevelStatements(speed->tokens, static_cast<long long>(initializer->bodyOpen), static_cast<long long>(initializer->bodyClose)))
            {
                const auto stmt = Copy(speed->tokens, a, b);
                if (stmt.size() > 3 && SliceIs(Lows(stmt, 0, 2), 0, 2, {"level", "."}) && reads.count(stmt[2].Low()))
                    initial.push_back(Emit(stmt));
            }
        const auto framework = rounds ? sources.Get(S(roundContract["script"])) : nullptr;
        if (const Function* begin = FunctionOf(framework, S(roundContract["start_function"])))
        {
            for (const auto& [a, b] : TopLevelStatements(framework->tokens, static_cast<long long>(begin->bodyOpen), static_cast<long long>(begin->bodyClose)))
            {
                const auto stmt = Copy(framework->tokens, a, b);
                if (At(stmt, 0).Low() == "while")
                    break;
                if (In(At(stmt, 0).Low(), {"for", "foreach"}))
                    continue;
                const bool direct = stmt.size() > 3 && SliceIs(Lows(stmt, 0, 2), 0, 2, {"level", "."}) && reads.count(stmt[2].Low());
                bool writers = false;
                for (const auto& ref : References(stmt))
                {
                    const auto dep = owner(*framework, ref);
                    if (!dep)
                        continue;
                    for (const auto& w : writes(*dep, Lower(ref.name)))
                        writers = writers || reads.count(w);
                }
                if (direct || writers)
                    start.push_back(Emit(stmt));
            }
        }
        if (const Function* finish = FunctionOf(framework, S(roundContract["end_function"])))
        {
            const auto& tokens = framework->tokens;
            int depth = 0;
            std::vector<std::optional<Tokens>> guards;
            const auto open = static_cast<long long>(finish->bodyOpen), close = static_cast<long long>(finish->bodyClose);
            for (long long i = open + 1; i < close - 3; i++)
            {
                depth += tokens[i].text == "{";
                depth -= tokens[i].text == "}";
                if (tokens[i].text == "{")
                {
                    std::optional<Tokens> header;
                    if (At(tokens, i - 1).text == ")")
                    {
                        int nesting = 1;
                        long long opening = i - 2;
                        while (opening > open && nesting)
                        {
                            nesting += At(tokens, opening).text == ")";
                            nesting -= At(tokens, opening).text == "(";
                            opening--;
                        }
                        if (At(tokens, opening).Low() == "if")
                            header = Copy(tokens, opening, i);
                    }
                    guards.push_back(header);
                }
                else if (tokens[i].text == "}" && !guards.empty())
                    guards.pop_back();
                if (!SliceIs(Lows(tokens, i, 2), 0, 2, {"level", "."}) || !reads.count(At(tokens, i + 2).Low()))
                    continue;
                if (!In(At(tokens, i + 3).text, {"=", "+=", "-=", "++", "--"}))
                    continue;
                long long boundary = open;
                for (long long j = open; j < i; j++)
                    if (In(tokens[j].text, {";", "{", "}"}))
                        boundary = std::max(boundary, j);
                bool control = false;
                for (const auto& w : LowsRange(tokens, boundary + 1, i))
                    control = control || In(w, {"if", "else", "for", "switch", "case"});
                bool unguarded = false;
                for (size_t g = 1; g < guards.size(); g++)
                    unguarded = unguarded || !guards[g];
                if (control || (depth > 1 && unguarded))
                    throw std::runtime_error(framework->path + "::" + finish->name + ": conditional source speed state needs a control-flow slice");
                long long j = i;
                while (j < close && tokens[j].text != ";")
                    j++;
                if (j >= close)
                    throw std::runtime_error("StopIteration");
                auto stmt = Copy(tokens, i, j + 1);
                if (depth == 0)
                {
                    initial.push_back(Emit(stmt));
                    continue;
                }
                std::vector<Tokens> headers;
                for (const auto& h : guards)
                    if (h)
                        headers.push_back(*h);
                if (!headers.empty())
                {
                    Tokens guarded;
                    for (const auto& h : headers)
                    {
                        guarded.insert(guarded.end(), h.begin(), h.end());
                        guarded.push_back(Punct("{"));
                    }
                    guarded.insert(guarded.end(), stmt.begin(), stmt.end());
                    for (size_t h = 0; h < headers.size(); h++)
                        guarded.push_back(Punct("}"));
                    stmt = std::move(guarded);
                }
                for (long long k = 0; k < static_cast<long long>(stmt.size()) - 2; k++)
                    if (SliceIs(Lows(stmt, k, 3), 0, 3, {"level", ".", "round_number"}))
                    {
                        stmt[k].text = "source_round";
                        stmt[k + 1].text = stmt[k + 2].text = "";
                    }
                end.push_back(Emit(stmt));
            }
        }
        if (initial.empty() && start.empty() && end.empty())
            return std::nullopt;
        std::set<std::string> includes{S(contract["script"]), S(roundContract["script"])};
        if (framework)
            includes.insert(framework->includes.begin(), framework->includes.end());
        std::vector<std::string> lines;
        for (const auto& path : includes)
            lines.push_back("#include " + path + ";");
        std::string out = Join(lines, "\n") + "\ninit()\n{\n" + Join(initial, "\n") + "\n}\n" + "\nprepare_round()\n{\n";
        if (!end.empty())
            out += "if (level.round_number > 1) {\nsource_round = level.round_number - 1;\n" + Join(end, "\n") + "\n}\n";
        return out + Join(start, "\n") + "\n}\n";
    }

    std::optional<std::string> SourceSelection(Sources& sources, const py::Json& definition)
    {
        const auto& c = definition["source_selection"];
        const auto script = sources.Get(S(c["script"]));
        const Function* fn = FunctionOf(script, S(c["function"]));
        if (!fn)
            return std::nullopt;
        const auto& tokens = script->tokens;
        const auto candidate = S(c["candidate"]), field = S(c["array_field"]), previous = S(c["previous"]);
        int depth = 0, base = 0;
        std::optional<long long> start, end;
        for (long long i = static_cast<long long>(fn->bodyOpen) + 1; i < static_cast<long long>(fn->bodyClose); i++)
        {
            depth += tokens[i].text == "{";
            depth -= tokens[i].text == "}";
            if (!start && SliceIs(Lows(tokens, i, 5), 0, 5, {candidate, "=", "level", ".", field}))
            {
                start = i;
                base = depth;
            }
            if (start && depth == base && SliceIs(Lows(tokens, i, 4), 0, 4, {previous, "=", candidate, ";"}))
            {
                end = i + 4;
                break;
            }
        }
        if (start && !end)
        {
            std::optional<long long> close;
            for (long long i = *start; i < static_cast<long long>(fn->bodyClose); i++)
                if (tokens[i].text == ";")
                {
                    close = i;
                    break;
                }
            if (close)
            {
                const Words expected{candidate, "=", "level", ".", field, "[", "randomint", "(", "level", ".", field, ".", "size", ")", "]", ";"};
                if (LowsRange(tokens, *start, *close + 1) == expected)
                    end = *close + 1;
            }
        }
        if (!start || !end)
            return std::nullopt;
        auto body = Copy(tokens, *start, *end);
        for (long long i = 0; i < static_cast<long long>(body.size()) - 2; i++)
            if (SliceIs(Lows(body, i, 3), 0, 3, {"level", ".", field}))
            {
                body[i].text = "candidates";
                body[i + 1].text = body[i + 2].text = "";
            }
        return "#include " + S(c["script"]) + ";" + "\nchoose(candidates, " + previous + ")\n{\n" + Emit(body) + "\nreturn " + candidate + ";\n}\n";
    }

    std::vector<std::pair<std::string, std::optional<std::set<long long>>>> ReachableVariants(Sources& sources, const py::Json& definition)
    {
        const auto& contract = definition["source_movement"];
        const auto script = sources.Get(contract.contains("variant_script") ? S(contract["variant_script"]) : "");
        std::vector<std::pair<std::string, std::optional<std::set<long long>>>> result;
        if (!script)
            return result;
        const auto& toks = script->tokens;
        const auto variant = S(contract["variant_function"]);
        static const pyre::Regex speedLiteral(R"re("(walk|run|sprint)")re");
        for (long long i = 0; i < static_cast<long long>(toks.size()) - 6; i++)
        {
            const auto words = Lows(toks, i, 7);
            if (!SliceIs(words, 0, 2, {variant, "("}) || words[3] != "+" || words[5] != ")")
                continue;
            const auto match = speedLiteral.FullMatch(words[2]);
            if (!match || toks[i + 4].kind != Kind::Ident)
                continue;
            const auto var = words[4];
            std::optional<std::set<long long>> picked;
            for (long long j = i - 1; j > 9; j--)
            {
                if (In(toks[j].text, {"{", "}"}))
                    break;
                const auto span = Lows(toks, j, 9);
                if (SliceIs(span, 0, 4, {var, "=", "randomintrange", "("}) && Word(span, 5) == "," && SliceIs(span, 7, 9, {")", ";"})
                    && toks[j + 4].kind == Kind::Number && toks[j + 6].kind == Kind::Number)
                {
                    std::set<long long> range;
                    for (long long v = IntOfFloat(span[4]); v < IntOfFloat(span[6]); v++)
                        range.insert(v);
                    picked = range;
                    break;
                }
            }
            const auto speed = (*match)[1];
            auto it = std::find_if(result.begin(), result.end(), [&](const auto& r) { return r.first == speed; });
            const bool exists = it != result.end();
            if (!picked || (exists && !it->second))
            {
                if (exists)
                    it->second = std::nullopt;
                else
                    result.emplace_back(speed, std::nullopt);
            }
            else
            {
                if (!exists)
                {
                    result.emplace_back(speed, std::set<long long>{});
                    it = result.end() - 1;
                }
                it->second->insert(picked->begin(), picked->end());
            }
        }
        return result;
    }

    std::optional<LastZombie> SourceLastZombie(Sources& sources, const py::Json& definition)
    {
        const auto& contract = definition["source_last_zombie"];
        const auto script = sources.Get(S(contract["script"]));
        const Function* fn = FunctionOf(script, S(contract["function"]));
        if (!fn)
            return std::nullopt;
        const auto& toks = script->tokens;
        const auto anim = S(contract["anim_function"]);
        for (long long i = static_cast<long long>(fn->bodyOpen); i < static_cast<long long>(fn->bodyClose); i++)
        {
            const auto words = Lows(toks, i, 8);
            if (words.size() < 8 || !SliceIs(words, 0, 5, {"if", "(", "level", ".", "round_number"}) || !In(words[5], {">", ">="})
                || At(toks, i + 6).kind != Kind::Number || words[7] != ")" || At(toks, i + 8).text != "{")
                continue;
            const auto close = static_cast<long long>(Match(toks, static_cast<size_t>(i + 8), "{", "}"));
            const auto body = LowsRange(toks, i + 8, close);
            bool last = false;
            for (long long k = 0; k < static_cast<long long>(body.size()); k++)
                last = last || SliceIs(body, k, k + 4, {".", "size", "==", "1"});
            std::set<std::string> speeds;
            for (long long k = 0; k < static_cast<long long>(body.size()) - 3; k++)
                if (SliceIs(body, k, k + 2, {anim, "("}))
                    for (const char* s : {"walk", "run", "sprint"})
                        if (body[k + 2] == std::string("\"") + s + "\"")
                            speeds.insert(s);
            if (last && speeds.size() == 1)
            {
                const auto threshold = IntOfFloat(words[6]);
                return LastZombie{words[5] == ">" ? threshold : threshold - 1, *speeds.begin()};
            }
        }
        return std::nullopt;
    }

    std::optional<bool> FoldLevelGuard(const Words& words, const std::string& mapName, const std::set<std::string>* assigned)
    {
        size_t pos = 0;
        const auto peek = [&](const size_t n = 0) -> const std::string*
        { return pos + n < words.size() ? &words[pos + n] : nullptr; };
        const auto is = [&](const size_t n, const char* text)
        {
            const auto* p = peek(n);
            return p && *p == text;
        };
        std::function<std::optional<bool>()> atom, conjunction, disjunction;
        atom = [&]() -> std::optional<bool>
        {
            if (is(0, "!"))
            {
                pos++;
                const auto value = atom();
                return value ? std::optional<bool>(!*value) : std::nullopt;
            }
            if (is(0, "("))
            {
                pos++;
                const auto value = disjunction();
                if (!is(0, ")"))
                    throw NoGuard{};
                pos++;
                return value;
            }
            if (is(0, "isdefined") && is(1, "(") && is(2, "level") && is(3, ".") && is(5, ")"))
            {
                const auto field = *peek(4);
                pos += 6;
                if (field == "script")
                    return true;
                if (assigned && !assigned->count(field))
                    return false;
                return std::nullopt;
            }
            if (is(0, "level") && is(1, ".") && is(2, "script") && (is(3, "==") || is(3, "!=")) && peek(4) && StartsWith(*peek(4), "\""))
            {
                const bool matched = Lower(mapName) == StripChars(*peek(4), "\"");
                const bool equal = is(3, "==");
                pos += 5;
                return equal ? matched : !matched;
            }
            throw NoGuard{};
        };
        conjunction = [&]() -> std::optional<bool>
        {
            std::vector<std::optional<bool>> values{atom()};
            while (is(0, "&&"))
            {
                pos++;
                values.push_back(atom());
            }
            bool unknown = false;
            for (const auto& v : values)
            {
                if (v && !*v)
                    return false;
                unknown = unknown || !v;
            }
            return unknown ? std::nullopt : std::optional<bool>(true);
        };
        disjunction = [&]() -> std::optional<bool>
        {
            std::vector<std::optional<bool>> values{conjunction()};
            while (is(0, "||"))
            {
                pos++;
                values.push_back(conjunction());
            }
            bool unknown = false;
            for (const auto& v : values)
            {
                if (v && *v)
                    return true;
                unknown = unknown || !v;
            }
            return unknown ? std::nullopt : std::optional<bool>(false);
        };
        std::optional<bool> value;
        try
        {
            value = disjunction();
        }
        catch (const NoGuard&)
        {
            return std::nullopt;
        }
        return pos == words.size() ? value : std::nullopt;
    }

    std::pair<std::optional<std::string>, MovementSets> MovementSetsOf(Sources& sources, const py::Json& definition, const std::optional<std::string>& mapName)
    {
        const auto& contract = definition["source_movement"];
        const auto script = sources.Get(S(contract["script"]));
        const Function* fn = FunctionOf(script, S(contract["function"]));
        const auto field = S(contract["field"]), row = "\"" + S(contract["row"]) + "\"";
        static const pyre::Regex aliasLiteral(R"re("(?:walk|run|sprint)\d+")re");
        static const pyre::Regex aliasParts(R"((walk|run|sprint)(\d+))");
        if (script && !fn)
        {
            std::vector<const Function*> candidates;
            const auto& tokens = script->tokens;
            for (const auto& [_, candidate] : script->functions)
            {
                if (!candidate.params.empty())
                    continue;
                for (long long i = static_cast<long long>(candidate.bodyOpen) + 1; i < static_cast<long long>(candidate.bodyClose) - 12; i++)
                {
                    const auto words = Lows(tokens, i, 13);
                    if (SliceIs(words, 0, 5, {"level", ".", field, "[", row}) && SliceIs(words, 5, 7, {"]", "["}) && SliceIs(words, 8, 11, {"]", "=", "%"})
                        && aliasLiteral.FullMatch(words[7]))
                    {
                        candidates.push_back(&candidate);
                        break;
                    }
                }
            }
            if (candidates.size() > 1)
                throw std::runtime_error(script->path + ": ambiguous movement table initializers");
            fn = candidates.empty() ? nullptr : candidates[0];
        }
        if (!fn)
            return {std::nullopt, {}};
        const auto tokens = FoldedBody(sources, *script, *fn, mapName);
        std::vector<std::pair<std::string, std::map<long long, std::string>>> found;
        int depth = 0;
        for (long long i = 1; i < static_cast<long long>(tokens.size()) - 1; i++)
        {
            depth += tokens[i].text == "{";
            depth -= tokens[i].text == "}";
            const auto words = Lows(tokens, i, 13);
            if (words.size() < 13 || !SliceIs(words, 0, 5, {"level", ".", field, "[", row}))
                continue;
            if (!SliceIs(words, 5, 7, {"]", "["}) || !SliceIs(words, 8, 11, {"]", "=", "%"}) || words[12] != ";")
                continue;
            const auto alias = StripChars(words[7], "\"");
            const auto match = aliasParts.FullMatch(alias);
            if (!match)
                continue;
            if (depth)
                throw std::runtime_error(S(contract["script"]) + ":" + std::to_string(tokens[i].line) + ": conditional movement table requires runtime translation");
            auto it = std::find_if(found.begin(), found.end(), [&](const auto& f) { return f.first == (*match)[1]; });
            if (it == found.end())
            {
                found.emplace_back((*match)[1], std::map<long long, std::string>{});
                it = found.end() - 1;
            }
            it->second[std::stoll((*match)[2])] = words[11];
        }
        const auto reachable = ReachableVariants(sources, definition);
        for (auto& [speed, entries] : found)
        {
            const auto r = std::find_if(reachable.begin(), reachable.end(), [&](const auto& x) { return x.first == speed; });
            if (r == reachable.end() || !r->second || r->second->empty())
                continue;
            bool overlap = false;
            for (const auto& [k, _] : entries)
                overlap = overlap || r->second->count(k);
            if (!overlap)
                continue;
            std::map<long long, std::string> kept;
            for (const auto& [k, v] : entries)
                if (r->second->count(k))
                    kept[k] = v;
            entries = kept;
        }
        MovementSets sets;
        for (const auto& [speed, entries] : found)
        {
            std::vector<std::string> clips;
            for (const auto& [_, clip] : entries)
                clips.push_back(clip);
            sets.emplace_back(speed, clips);
        }
        return {fn->animtree, sets};
    }

    MovementSets MeleeSets(Sources& sources, const py::Json& definition, const std::optional<std::string>& mapName)
    {
        const auto& contract = definition["source_melee"];
        const auto script = sources.Get(S(contract["script"]));
        if (!script)
            return {};
        std::set<std::string> fields;
        for (const auto& [_, names] : contract["states"].items())
            for (const auto& n : names)
                fields.insert(S(n));
        const Function* fn = script->Find(S(contract["function"]));
        const auto& toks = script->tokens;
        if (!fn)
            for (const auto& [_, c] : script->functions)
            {
                if (!c.params.empty())
                    continue;
                bool assigns = false;
                for (long long i = static_cast<long long>(c.bodyOpen) + 1; i < static_cast<long long>(c.bodyClose) - 3 && !assigns; i++)
                    if (toks[i + 1].text == ".")
                        assigns = toks[i].Low() == "level" && fields.count(toks[i + 2].Low());
                if (assigns)
                {
                    fn = &c;
                    break;
                }
            }
        if (!fn)
            return {};
        std::map<std::string, std::map<long long, std::string>> tables;
        int depth = 0;
        const auto tokens = FoldedBody(sources, *script, *fn, mapName);
        const auto row = "\"" + S(contract["row"]) + "\"";
        for (long long i = 1; i < static_cast<long long>(tokens.size()) - 1; i++)
        {
            depth += tokens[i].text == "{";
            depth -= tokens[i].text == "}";
            const auto words = Lows(tokens, i, 13);
            if (words.size() < 13 || !SliceIs(words, 0, 2, {"level", "."}) || !fields.count(words[2]))
                continue;
            if (!SliceIs(words, 3, 7, {"[", row, "]", "["}) || !SliceIs(words, 8, 11, {"]", "=", "%"}) || words[12] != ";" || !IsDigits(words[7]))
                continue;
            if (depth)
                return {};
            tables[words[2]][std::stoll(words[7])] = words[11];
        }
        MovementSets states;
        for (const auto& [state, names] : contract["states"].items())
        {
            std::vector<std::string> clips;
            for (const auto& name : names)
                if (const auto t = tables.find(S(name)); t != tables.end())
                    for (const auto& [_, clip] : t->second)
                        clips.push_back(clip);
            if (!clips.empty())
                states.emplace_back(state, clips);
        }
        return states;
    }

    std::tuple<std::string, std::vector<std::string>, std::vector<std::string>> SourceTraversalClips(std::string asd, const FindXanim& find, const fs::path& outRoot)
    {
        std::set<std::string> staged, native;
        for (const char* state : {"zm_traverse", "zm_traverse_crawl", "zm_traverse_barrier", "zm_traverse_barrier_crawl"})
        {
            const pyre::Regex pattern("^" + pyre::Escape(state) + R"(\s*:[^\n]*\n\{(.*?)^\})", pyre::MULTILINE | pyre::DOTALL);
            const auto block = pattern.Search(asd);
            if (!block)
                continue;
            std::vector<std::string> lines;
            for (const auto& line : SplitOn((*block)[1], '\n'))
            {
                const auto words = SplitWhitespace(line);
                if (words.size() != 2 || StartsWith(PyLStrip(line), "//"))
                {
                    lines.push_back(line);
                    continue;
                }
                const auto clip = words[1];
                const auto src = find(clip);
                if (!src)
                {
                    native.insert(clip);
                    lines.push_back(line);
                    continue;
                }
                const auto asset = "waw_anim_" + Lower(clip);
                const auto dst = outRoot / "xanim" / files::FromUtf8(asset);
                std::error_code error;
                if (!fs::is_regular_file(dst, error))
                {
                    fs::create_directories(dst.parent_path());
                    fs::copy_file(*src, dst, fs::copy_options::overwrite_existing);
                }
                staged.insert(asset);
                const auto at = line.rfind(clip);
                lines.push_back(line.substr(0, at) + asset + line.substr(at + clip.size()));
            }
            const auto a = ByteOffset(asd, block->Start(1)), b = ByteOffset(asd, block->End(1));
            asd = asd.substr(0, a) + Join(lines, "\n") + asd.substr(b);
        }
        return {asd, std::vector<std::string>(staged.begin(), staged.end()), std::vector<std::string>(native.begin(), native.end())};
    }

    py::Json StageActorMovement(Sources& sources, const fs::path& outRoot, const fs::path& bo2Root, const py::Json& definition,
                                const std::optional<std::string>& mapName, const FindXanim* find)
    {
        auto sets = MovementSetsOf(sources, definition, mapName).second;
        auto asd = files::ReadText(bo2Root / "raw/animstatedefs/zm_nuked_basic.asd");
        std::vector<std::string> traversal, traversalNative;
        if (find)
            std::tie(asd, traversal, traversalNative) = SourceTraversalClips(asd, *find, outRoot);
        if (sets.empty() && traversal.empty())
            return py::Json::object();
        std::vector<std::string> clips(traversal), missing, native;
        const auto has = [&](const std::string& name)
        {
            std::error_code error;
            return fs::is_regular_file(outRoot / "xanim" / files::FromUtf8("waw_anim_" + name), error);
        };
        MovementSets kept;
        for (const auto& [speed, names] : sets)
        {
            std::vector<std::string> present;
            for (const auto& name : names)
                if (has(name))
                    present.push_back(name);
            for (const auto& name : names)
                if (std::find(present.begin(), present.end(), name) == present.end())
                    missing.push_back(name);
            if (present.empty())
            {
                native.push_back(speed);
                continue;
            }
            kept.emplace_back(speed, present);
            std::vector<std::string> assets;
            for (const auto& name : present)
                assets.push_back("waw_anim_" + name);
            asd = ReplaceState(asd, S(definition["source_movement"]["states"][speed]), assets);
            clips.insert(clips.end(), assets.begin(), assets.end());
        }
        sets = kept;
        MovementSets melee;
        std::vector<std::string> meleeNative;
        for (const auto& [state, names] : MeleeSets(sources, definition, mapName))
        {
            std::vector<std::string> absent;
            for (const auto& name : names)
                if (!has(name))
                    absent.push_back(name);
            if (!absent.empty())
            {
                missing.insert(missing.end(), absent.begin(), absent.end());
                meleeNative.push_back(state);
                continue;
            }
            std::vector<std::string> assets;
            for (const auto& name : names)
                assets.push_back("waw_anim_" + name);
            asd = ReplaceState(asd, state, assets);
            clips.insert(clips.end(), assets.begin(), assets.end());
            melee.emplace_back(state, names);
        }
        const auto tree = outRoot / "animtrees" / (std::string(SCRIPTED_TREE) + ".atr");
        std::set<std::string> scriptedSet;
        std::error_code error;
        if (fs::is_regular_file(tree, error))
            for (const auto line : py::SplitLines(files::ReadText(tree)))
            {
                const std::string stripped(py::Strip(line));
                if (!stripped.empty() && !StartsWith(line, "//"))
                    scriptedSet.insert(stripped);
            }
        const std::vector<std::string> scripted(scriptedSet.begin(), scriptedSet.end());
        for (size_t start = 0; start < scripted.size(); start += SCRIPTED_CHUNK)
        {
            std::string block = RStripNewlines(asd) + "\n\n" + SCRIPTED_STATE + "_" + std::to_string(start / SCRIPTED_CHUNK) + " : aliased restart notify " + SCRIPTED_NOTIFY
                                + "\n{\n";
            for (size_t k = start; k < std::min(scripted.size(), start + SCRIPTED_CHUNK); k++)
                block += "\tc" + std::to_string(k) + "\t" + scripted[k] + "\n";
            asd = block + "}\n";
        }
        clips.insert(clips.end(), scripted.begin(), scripted.end());
        const std::set<std::string> unique(clips.begin(), clips.end());
        auto atr = files::ReadText(bo2Root / "raw/animtrees/zm_nuked_basic.atr");
        static const pyre::Regex body(R"((^body\s*\n\{))", pyre::MULTILINE);
        size_t count = 0;
        atr = body.Sub(
            [&](const pyre::Match& m)
            {
                std::string text = m[1] + "\n";
                for (const auto& c : unique)
                    text += "\t" + c + "\n";
                return text;
            },
            atr, 1, &count);
        if (count != 1)
            throw std::runtime_error("Native zombie animation tree has no body group");
        files::WriteText(outRoot / "animtrees" / (std::string(ACTOR_TREE) + ".atr"), atr);
        files::WriteText(outRoot / "animstatedefs" / (std::string(ACTOR_TREE) + ".asd"), asd);
        static const pyre::Regex refs(R"(\n// BEGIN SOURCE MOVEMENT REFS.*?// END SOURCE MOVEMENT REFS\n)", pyre::DOTALL);
        static const pyre::Regex actorName(R"(waw_.*\..sc)", pyre::DOTALL | pyre::IGNORECASE);
        const auto aitype = outRoot / "aitype";
        if (fs::is_directory(aitype, error))
            for (const auto& entry : fs::recursive_directory_iterator(aitype))
            {
                if (!entry.is_regular_file() || !actorName.FullMatch(files::Utf8(entry.path().filename())))
                    continue;
                auto text = files::ReadText(entry.path());
                if (!Contains(text, "zm_nuked_basic") && !Contains(text, ACTOR_TREE))
                    continue;
                text = Replace(text, "zm_nuked_basic", ACTOR_TREE);
                text = refs.Sub("", text);
                text += "\n// BEGIN SOURCE MOVEMENT REFS\nwaw_reference_movement_anims()\n{\n";
                for (const auto& c : unique)
                    text += "    dummy_anim_ref = %" + c + ";\n";
                text += "}\n// END SOURCE MOVEMENT REFS\n";
                files::WriteText(entry.path(), text);
            }
        py::Json result = py::Json::object();
        result["states"] = py::Json::object();
        for (const auto& [speed, names] : sets)
            result["states"][speed] = names;
        result["melee"] = py::Json::object();
        for (const auto& [state, names] : melee)
            result["melee"][state] = names;
        result["tree"] = ACTOR_TREE;
        result["clips"] = std::vector<std::string>(unique.begin(), unique.end());
        if (!traversal.empty())
            result["traversal_clips"] = traversal;
        if (!traversalNative.empty())
            result["native_traversal_clips"] = traversalNative;
        if (!missing.empty())
        {
            const std::set<std::string> m(missing.begin(), missing.end());
            result["missing"] = std::vector<std::string>(m.begin(), m.end());
        }
        if (!native.empty())
            result["native_states"] = native;
        if (!meleeNative.empty())
            result["native_melee_states"] = meleeNative;
        return result;
    }

    namespace
    {
        constexpr std::string_view RUNTIME_TEMPLATE = R"GSC(// waw2bo2: source WaW speed rules and plain spawner origin ownership.
init()
{{
    level.waw_source_spawn_membership = {source_membership};
    // BO2's {speed_up} sprints the last zombie every round past 3, then the
    // source speed state restores it: its clip re-rolled every 0.5 s.
    level.{speed_up} = ::source_last_zombie;
    {respawn_init}
    {state_init}
    // The map's own spawn loop when the port runs it (gscport extract_round_loop);
    // else native initialization can register a cached stock round-spawn pointer:
    // keep authored custom providers, route the native default to our copy.
    if ( isdefined( level.waw2bo2_source_round_spawning ) )
        level.round_spawn_func = level.waw2bo2_source_round_spawning;
    else if ( !isdefined( level.round_spawn_func ) || level.round_spawn_func == {native_round}::round_spawning )
        level.round_spawn_func = {owned_round}::round_spawning;
    bound = 0;
    spawners = getentarray( "zombie_spawner", "script_noteworthy" );
    foreach ( spawner in spawners )
    {{
        if ( !isdefined( spawner.spawn_funcs ) )
            continue;
        kept = [];
        for ( i = 0; i < spawner.spawn_funcs.size; i++ )
        {{
            if ( spawner.spawn_funcs[i]["function"] == {native}::{entry} ||
                 spawner.spawn_funcs[i]["function"] == {owned}::{entry} )
            {{
                bound++;
                {bo2_spawn_init}
            }}
            kept[kept.size] = spawner.spawn_funcs[i];
        }}
        spawner.spawn_funcs = kept;
    }}
    println( "WAW2BO2 zombie spawn callbacks adapted: " + bound );
}}

// T6 actor setup the map's zombie_spawn_init leaves to WaW's engine (gsc_api
// zombie_runtime actor_glue runs it first there). No game rules here.
waw2bo2_zombie_actor_glue()
{{
    self.team = level.zombie_team;
    // T6 reaction/pain animscripts; WaW's zombie AI had none
    self.a.disablepain = 1;
    // T4 actors start with the native pain state on: disable_pain only skips
    // the pain clip, so a nonfatal hit still cancels the swing (the stall).
    self.allowpain = 1;
    // A pain state restarts T6's move animscript, whose moverun re-rolls the
    // movement substate (a new walk clip every hit). T4 kept the actor's run
    // animation through pain; T6 keeps the substate when this flag is set and
    // the state is unchanged (zm_run::setanimstatefromspeed).
    self.preserve_asd_substates = 1;
    self maps\mp\zombies\_zm_utility::disable_react();
    // the template's actor capsule (72 tall broke upstairs windows)
    self setphysparams( 15, 0, 48 );
    // WaW engine zombie melee: 60 (ai melee weapon)
    self.meleedamage = 60;
    // T4's engine dispatches actor damage to the map's global callbacks.
    self thread waw2bo2_actor_damage();
    self thread waw2bo2_actor_sound_cleanup();
    self thread waw2bo2_actor_kill_credit();
    self thread waw2bo2_zombie_animstate();
}}

// T4's engine credited an actor's killer itself: the scoreboard's kills and
// headshots columns read the player's kills/headshots fields, which no WaW
// script writes. T6 leaves that to BO2's spawner (zombie_death_event), which
// the map's own spawner replaces.
waw2bo2_actor_kill_credit()
{{
    self waittill( "death", attacker );
    if ( !isdefined( attacker ) && isdefined( self ) )
        attacker = self.attacker;
    if ( !isdefined( attacker ) || !isplayer( attacker ) )
        return;
    attacker.kills++;
    if ( isdefined( self ) && isdefined( self.damagelocation ) && isdefined( self.damagemod ) &&
         self.damagemod != "MOD_MELEE" && ( self.damagelocation == "head" || self.damagelocation == "helmet" ) )
        attacker.headshots++;
}}

waw2bo2_actor_damage()
{{
    self endon( "death" );
    for ( ;; )
    {{
        self waittill( "damage", amount, attacker, direction, point, mod );
        if ( !isdefined( amount ) || !isalive( self ) )
            continue;
        if ( !isdefined( attacker ) || !isplayer( attacker ) )
            continue;
        if ( isdefined( level.waw_actor_damage_filter ) &&
             self [[ level.waw_actor_damage_filter ]]( mod, self.damagelocation, point, attacker, amount ) )
            continue;
        callback = level.waw_global_damage_func;
        if ( maps\mp\zombies\_zm_utility::isads( attacker ) && (mod == "MOD_RIFLE_BULLET" || mod == "MOD_PISTOL_BULLET") )
            callback = level.waw_global_damage_func_ads;
        if ( isdefined( callback ) )
            self [[ callback ]]( mod, self.damagelocation, point, attacker );
    }}
}}

// T6 leaves one-shot entity voices playing after actor death. T4 stopped
// those voices; the map's death vocal is a separate positional sound.
waw2bo2_actor_sound_cleanup()
{{
    self waittill( "death" );
    if ( isdefined( self ) )
        self stopsounds();
}}

// T6 picks a zombie's movement and death from its animation states
// (zm_move_<speed>, zm_death); its move animscript re-reads them only when
// told. WaW's engine read the run cycle the map's set_zombie_run_cycle set.
waw2bo2_zombie_animstate()
{{
    self endon( "death" );
    speed = undefined;
    legs = undefined;
    for ( ;; )
    {{
        if ( !isdefined( speed ) || !isdefined( self.zombie_move_speed ) || speed != self.zombie_move_speed ||
             !isdefined( legs ) || legs != self.has_legs )
        {{
            speed = self.zombie_move_speed;
            legs = self.has_legs;
            self.deathanim = self maps\mp\animscripts\zm_utility::append_missing_legs_suffix( "zm_death" );
            if ( isdefined( speed ) )
                self maps\mp\animscripts\zm_run::needsupdate();
        }}
        wait 0.05;
    }}
}}

bind_spawn_location()
{{
    if ( isdefined( self.spawn_point_override ) )
        return;
    // Plain WaW spawners use their own origin.
    if ( isdefined( self.script_string ) && self.script_string == "{riser_actor_value}" )
    {{
        // WaW do_zombie_rise: a riser rises at random( level.{rise_field} )
        // when the map keeps that list (Shi No Numa: the rise structs of the
        // zones players are in), as choose_spawn_location does for round
        // spawns. Source scripts also spawn risers themselves (Shi No Numa's
        // <hut>_initial_zombies when a door opens): with no point chosen, T6
        // would take any active zone location. With the list empty or absent
        // WaW's riser stays at its spawner.
        if ( isdefined( level.{rise_field} ) && level.{rise_field}.size )
        {{
            self.spawn_point_override = rise_point( level.{rise_field}[randomint( level.{rise_field}.size )], self );
            return;
        }}
        point = spawnstruct();
        point.origin = self.origin;
        point.angles = self.angles;
        point.script_noteworthy = "{plain_spawn_location}";
        point.target = self.target;
        point.targetname = self.targetname;
        self.spawn_point_override = point;
        return;
    }}
    // WaW DoSpawn: the zombie appears at its spawner (the map's own zone
    // manager chose the spawner); not a location from BO2's zone lists.
    point = spawnstruct();
    point.origin = self.origin;
    point.angles = self.angles;
    point.script_noteworthy = "{plain_spawn_location}";
    point.target = self.target;
    point.targetname = self.targetname;
    // T6 reads any other string as a barrier id (choose_spawn_location)
    if ( isdefined( self.script_string ) && self.script_string == "{source_chaser_value}" )
        point.script_string = "{native_chaser_value}";
    self.spawn_point_override = point;
}}

rise_point( anchor, actor )
{{
    point = spawnstruct();
    point.origin = anchor.origin;
    point.angles = anchor.angles;
    point.script_noteworthy = "{native_riser_location}";
    // T6 reads any other string as a barrier id; WaW riser_door goes to the
    // nearest windows, the native no-string default.
    if ( isdefined( anchor.script_noteworthy ) && anchor.script_noteworthy == "{native_chaser_value}" )
        point.script_string = "{native_chaser_value}";
    point.target = actor.target;
    point.targetname = actor.targetname;
    return point;
}}

spawn_zombie( spawner, target_name, spawn_point, round_number )
{{
    // T6's helper accepts spawn_point but does not use it. Its callbacks run
    // after a frame boundary, so bind the chosen active location first.
    ai = maps\mp\zombies\_zm_utility::spawn_zombie( spawner, target_name, spawn_point, round_number );
    if ( isdefined( ai ) && isdefined( spawn_point ) )
    {{
        ai.spawn_point_override = spawn_point;
        ai forceteleport( spawn_point.origin );
    }}
    return ai;
}}

select_speed()
{{
    if ( !isdefined( level.waw_speed_round ) || level.waw_speed_round != level.round_number )
    {{
        {state_round}
        level.waw_speed_round = level.round_number;
    }}
    {speed}
}}

source_last_zombie()
{{
{last_rule}
}}

refresh_speed( native_request )
{{
    if ( isdefined( self.{result_field} ) )
        native_request = self.{result_field};
    self maps\mp\zombies\_zm_utility::set_zombie_run_cycle( native_request );
}}

source_animstate( animstate, substate )
{{
    // Cached native scripts can still request another speed. Restore the source
    // field and its animation state when native movement consumes that request.
    if ( isdefined( self.{result_field} ) && self.zombie_move_speed != self.{result_field} )
    {{
        self refresh_speed();
        animstate = self maps\mp\animscripts\zm_utility::append_missing_legs_suffix( "zm_move_" + self.{result_field} );
        if ( isdefined( self.a.gib_ref ) && self.a.gib_ref == "no_legs" )
            animstate = "zm_move_stumpy";
        substate = undefined;
        self setanimstatefromasd( animstate );
    }}
    if ( isdefined( self.waw_previous_speed_callback ) )
        self [[ self.waw_previous_speed_callback ]]( animstate, substate );
}}

choose_spawner( candidates, spawn_point )
{{
    if ( isdefined( spawn_point ) && isdefined( spawn_point.waw_source_spawner ) )
        return spawn_point.waw_source_spawner;
    // Synthesized locations retain each WaW actor's origin. Choose its source
    // actor when available instead of an actor in a closed or distant zone.
    if ( isdefined( spawn_point ) )
    {{
        matching = [];
        foreach ( candidate in candidates )
            if ( distancesquared( candidate.origin, spawn_point.origin ) < 1.0 )
                matching[matching.size] = candidate;
        if ( matching.size )
            candidates = matching;
    }}
    if ( !isdefined( level.waw_selection_round ) || level.waw_selection_round != level.round_number )
    {{
        level.waw_previous_spawner = undefined;
        level.waw_selection_round = level.round_number;
    }}
    chosen = {selection};
    level.waw_previous_spawner = chosen;
    return chosen;
}}

spawn_locations_available()
{{
    if ( isdefined( level.waw_source_spawn_membership ) && level.waw_source_spawn_membership )
        return isdefined( level.{member_field} ) && level.{member_field}.size > 0;
    return level.zombie_spawn_locations.size > 0;
}}

choose_spawn_location()
{{
    if ( !isdefined( level.waw_source_spawn_membership ) || !level.waw_source_spawn_membership )
        return level.zombie_spawn_locations[randomint( level.zombie_spawn_locations.size )];
    actor = choose_spawner( level.{member_field}, undefined );
    anchor = actor;
    noteworthy = "spawn_location";
    if ( isdefined( actor.script_string ) && actor.script_string == "{riser_actor_value}" )
    {{
        if ( !isdefined( level.{rise_field} ) || !level.{rise_field}.size )
            return undefined;
        anchor = level.{rise_field}[randomint( level.{rise_field}.size )];
        noteworthy = "{native_riser_location}";
    }}
    point = spawnstruct();
    point.origin = anchor.origin;
    point.angles = anchor.angles;
    point.script_noteworthy = noteworthy;
    // T6 interprets any other string as a specific barrier id. WaW's
    // riser_door and ordinary actors use the nearest-window path instead.
    if ( noteworthy == "{native_riser_location}" )
    {{
        if ( isdefined( anchor.script_noteworthy ) && anchor.script_noteworthy == "{native_chaser_value}" )
            point.script_string = "{native_chaser_value}";
    }}
    else if ( isdefined( actor.script_string ) && actor.script_string == "{source_chaser_value}" )
        point.script_string = "{native_chaser_value}";
    point.target = actor.target;
    point.targetname = actor.targetname;
    point.waw_source_spawner = actor;
    return point;
}}

source_chases( rise_result )
{{
    {chase}
}}
{respawn_body}
{scripted})GSC";

        constexpr std::string_view RESPAWN_TEMPLATE = R"GSC(source_respawn( player, return_struct )
{{
    origin = {respawn_function}( player );
    if ( !isdefined( origin ) || !isdefined( return_struct ) || !return_struct )
        return origin;
    point = spawnstruct();
    point.origin = origin;
    point.angles = ( 0, 0, 0 );
    return point;
}}
)GSC";
    } // namespace

    std::string RuntimeSource(const py::Json& definition, const std::optional<std::string>& speedFunction, const std::optional<std::string>& selectionFunction,
                              const std::optional<std::string>& speedStateFunction, const bool sourceMembership,
                              const std::optional<std::string>& respawnFunction, const std::optional<std::string>& chaseFunction,
                              const std::optional<LastZombie>& lastZombie, std::vector<std::string> scriptedClips)
    {
        const auto native = S(definition["native_spawner"]);
        std::sort(scriptedClips.begin(), scriptedClips.end());
        std::string scripted = scriptedClips.empty() ? "\n" : "\n#using_animtree( \"" + std::string(SCRIPTED_TREE) + "\" );\n\n";
        scripted += "waw_scripted_index( clip )\n{\n";
        for (size_t k = 0; k < scriptedClips.size(); k++)
            scripted += "    if ( clip == %" + scriptedClips[k] + " )\n        return " + std::to_string(k) + ";\n";
        scripted += "    return undefined;\n}\n";
        const auto preserved = ApiSection("preserved_scripts");
        bool sourceAi = false;
        for (const auto& p : preserved)
            sourceAi = sourceAi || (p.is_string() && p.get<std::string>() == S(definition["source_chase"]["script"]));
        const auto owned = S(definition["owned_spawner"]);
        const auto entry = S(definition["native_spawn_init"]);
        const auto bo2SpawnInit = sourceAi ? std::string("continue;  // the map's own zombie_spawn_init runs (WaW drives the zombie AI)")
                                           : "spawner.spawn_funcs[i][\"function\"] = " + owned + "::" + entry + ";";
        const auto resultField = S(definition["source_speed"]["owned_result_field"]);
        const auto callbackField = S(definition["native_movement"]["callback"]);
        const auto& membership = definition["source_membership"];
        const auto& respawn = definition["source_respawn"];
        std::map<std::string, std::string> values;
        values["source_membership"] = sourceMembership ? "1" : "0";
        values["speed_up"] = S(definition["source_last_zombie"]["native_callback"]);
        values["respawn_init"] = respawnFunction ? "level." + S(respawn["native_callback"]) + " = ::source_respawn;" : "";
        values["state_init"] = speedStateFunction ? *speedStateFunction + "::init();" : "";
        values["native_round"] = S(definition["native_round_runtime"]);
        values["owned_round"] = S(definition["owned_round_runtime"]);
        values["native"] = native;
        values["entry"] = entry;
        values["owned"] = owned;
        values["bo2_spawn_init"] = bo2SpawnInit;
        values["riser_actor_value"] = S(membership["riser_actor_value"]);
        values["rise_field"] = S(membership["riser_array_field"]);
        values["plain_spawn_location"] = S(definition["plain_spawn_location"]);
        values["source_chaser_value"] = S(membership["source_chaser_value"]);
        values["native_chaser_value"] = S(membership["native_chaser_value"]);
        values["native_riser_location"] = S(membership["native_riser_location"]);
        values["state_round"] = speedStateFunction ? *speedStateFunction + "::prepare_round();" : "";
        values["speed"] = speedFunction ? "self " + *speedFunction + "();\n" + "    self refresh_speed();\n" + "    if ( !isdefined( self.waw_speed_callback_bound ) )\n"
                                              + "    {\n" + "        self.waw_speed_callback_bound = 1;\n" + "        self.waw_previous_speed_callback = self."
                                              + callbackField + ";\n" + "        self." + callbackField + " = ::source_animstate;\n" + "    }"
                                        : "self maps\\mp\\zombies\\_zm_utility::init_zombie_run_cycle();";
        if (lastZombie)
            values["last_rule"] = "    if ( level.round_number <= " + std::to_string(lastZombie->afterRound) + " || level.zombie_total > 0 )\n        return;\n"
                                  + "    if ( isdefined( self." + resultField + " ) && self." + resultField + " == \"" + lastZombie->speed + "\" )\n        return;\n"
                                  + "    self." + resultField + " = \"" + lastZombie->speed + "\";\n    self refresh_speed();";
        else
            values["last_rule"] = "    // the source keeps the last zombie at its own speed";
        values["result_field"] = resultField;
        values["selection"] = selectionFunction ? *selectionFunction + "( candidates, level.waw_previous_spawner )" : "common_scripts\\utility::random( candidates )";
        values["member_field"] = S(definition["source_selection"]["array_field"]);
        values["chase"] = chaseFunction ? "return self " + *chaseFunction + "( rise_result );" : "return false;";
        values["respawn_body"] = respawnFunction ? Format(RESPAWN_TEMPLATE, {{"respawn_function", *respawnFunction}}) : "";
        values["scripted"] = scripted;
        return Format(RUNTIME_TEMPLATE, values);
    }

    namespace
    {
        const std::string DROP_GATE = "    if ( !isdefined( attacker ) || !isplayer( attacker ) )\n        return;\n\n    if ( zombie_can_drop_powerups( zombie ) )";
        const std::string KILL_VOX = "    level thread maps\\mp\\zombies\\_zm_audio::player_zombie_kill_vox( hit_location, attacker, mod, zombie );";
        const std::string DROP_CALL = "    level thread maps\\mp\\zombies\\_zm_powerups::powerup_drop( origin );";
        const std::string WAW_DROP_CHECK = R"GSC(
waw_zombie_can_drop_powerups( zombie )
{
    if ( is_tactical_grenade( zombie.damageweapon ) || !flag( "zombie_drop_powerups" ) )
        return false;

    // waw2bo2: WaW gives no points or power-ups for zombies marked for death
    if ( isdefined( zombie.marked_for_death ) && zombie.marked_for_death )
        return false;

    return true;
}
)GSC";
        const std::string DEATH_EVENT = "    level thread zombie_death_event( self );\n";
        const std::string DEATH_VOCALS = "    zombie thread maps\\mp\\zombies\\_zm_audio::do_zombies_playvocals( \"death\", name );\n";
        const std::string KILL_NOTIFY = "        zombie.attacker notify( \"zom_kill\", zombie );\n";
        const std::string CLOSE_DAMAGE_VOX = R"GSC(        if ( isdefined( zombie.sound_damage_player ) && zombie.sound_damage_player == zombie.attacker )
        {
            chance = get_response_chance( "damage" );

            if ( chance != 0 )
            {
                if ( chance > randomintrange( 1, 100 ) )
                    zombie.attacker maps\mp\zombies\_zm_audio::create_and_play_dialog( "kill", "damage" );
            }
            else
                zombie.attacker maps\mp\zombies\_zm_audio::create_and_play_dialog( "kill", "damage" );
        }
)GSC";
    } // namespace

    std::string WawQuoteRules(std::string text, const std::string& native, const std::string& coreText)
    {
        if (!Contains(coreText, "waw2bo2_zombie_death_event("))
            return text;
        for (const auto* anchor : {&DEATH_EVENT, &DEATH_VOCALS, &KILL_NOTIFY, &CLOSE_DAMAGE_VOX})
            if (Count(text, *anchor) != 1)
                throw std::runtime_error(native + ": zombie death event contract changed near " + PyStripRepr40(*anchor));
        text = Replace(text, DEATH_EVENT, DEATH_EVENT + "    maps\\mp\\waw\\_waw2bo2_core::waw2bo2_zombie_death_event( self );\n");
        for (const auto* anchor : {&DEATH_VOCALS, &KILL_NOTIFY, &CLOSE_DAMAGE_VOX})
            text = Replace(text, *anchor, "");
        return text;
    }

    std::string WawDropRules(std::string text, const std::string& native)
    {
        for (const auto* anchor : {&DROP_GATE, &KILL_VOX, &DROP_CALL})
            if (Count(text, *anchor) != 1)
                throw std::runtime_error(native + ": power-up drop contract changed near " + PyStripRepr40(*anchor));
        text = Replace(text, DROP_GATE, "    if ( waw_zombie_can_drop_powerups( zombie ) )");
        text = Replace(text, KILL_VOX, "    if ( !isdefined( attacker ) || !isplayer( attacker ) )\n        return;\n\n" + KILL_VOX);
        text = Replace(text, DROP_CALL, "    level thread maps\\mp\\waw\\_waw2bo2_compat::waw_powerup_drop( origin );");
        return text + WAW_DROP_CHECK;
    }

    void StageNativeSupport(const fs::path& outRoot, const fs::path& bo2Root, const py::Json& definition)
    {
        std::error_code error;
        if (!fs::is_regular_file(outRoot / files::FromUtf8(Replace(RUNTIME, "\\", "/") + ".gsc"), error))
            return;
        const auto native = S(definition["native_spawner"]), owned = S(definition["owned_spawner"]);
        const auto source = files::ReadText(bo2Root / "raw" / files::FromUtf8(Replace(native, "\\", "/") + ".gsc"));
        auto script = Parse(source, native);
        auto& tokens = script.tokens;
        const bool hasPolicy = fs::is_regular_file(outRoot / "maps/mp/waw/_waw2bo2_source_chase_policy.gsc", error);
        const Function* skip = script.Find("should_skip_teardown");
        if (hasPolicy && !skip)
            throw std::runtime_error(native + ": missing chase/entrance routing predicate");
        if (hasPolicy)
        {
            if (skip->params.size() != 1)
                throw std::runtime_error(native + ": expected one chase/entrance event argument");
            tokens[skip->bodyOpen].text += "\n    return self " + std::string(RUNTIME) + "::source_chases( " + skip->params[0] + " );\n";
        }
        const Function* think = script.Find("zombie_think");
        if (hasPolicy && think)
        {
            const auto result = RiseResult(script, *think, S(definition["source_chase"]["rise_event"]));
            if (!result)
                throw std::runtime_error(native + ": missing spawn-completion event result");
            for (long long i = static_cast<long long>(think->bodyOpen) + 1; i < static_cast<long long>(think->bodyClose) - 2; i++)
            {
                if (tokens[i].Low() != "if" || tokens[i + 1].text != "(")
                    continue;
                const auto end = static_cast<long long>(Match(tokens, static_cast<size_t>(i + 1), "(", ")"));
                std::string cond;
                for (long long k = i + 2; k < end; k++)
                    cond += tokens[k].Low();
                if (cond == "!isdefined(" + Lower(*result) + ")&&isdefined(self.target)&&self.target!=\"\"")
                {
                    tokens[end].text = " && !self " + std::string(RUNTIME) + "::source_chases( " + *result + " ) )";
                    const auto chaser = S(definition["source_membership"]["native_chaser_value"]);
                    tokens[i].text = "if ( isdefined( " + *result + " ) && " + *result + " == \"" + chaser + "\" && !self " + RUNTIME + "::source_chases( " + *result
                                     + " ) )\n        " + *result + " = undefined;\n\n    if";
                    break;
                }
            }
        }
        const auto core = outRoot / "maps/mp/waw/_waw2bo2_core.gsc";
        const bool hasCore = fs::is_regular_file(core, error);
        if (hasCore && Contains(files::ReadText(core), "waw_tesla_init()"))
        {
            const Function* death = script.Find("zombie_death_points");
            if (!death || std::find(death->params.begin(), death->params.end(), "zombie") == death->params.end())
                throw std::runtime_error(native + ": Tesla needs the native zombie death-points contract");
            tokens[death->bodyOpen].text += "\n    if ( isdefined( zombie.tesla_death ) && zombie.tesla_death )\n        return;\n";
        }
        const Function* fn = script.Find(S(definition["native_spawn_init"]));
        const Function* location = script.Find(S(definition["native_location_function"]));
        if (!fn || !location)
            throw std::runtime_error(native + ": required spawn callback/location function missing");
        std::vector<Ref> refs;
        for (const auto& r : References(tokens, fn->bodyOpen, fn->bodyClose))
            if (Lower(r.name) == S(definition["native_speed_init"]) && !r.pointer)
                refs.push_back(r);
        if (refs.size() != 1)
            throw std::runtime_error(native + ": expected one speed initialization before zombie_think");
        const auto ref = refs[0];
        for (const auto& r : References(tokens, fn->bodyOpen, fn->bodyClose))
            if (Lower(r.name) == "zombie_think" && r.index < ref.index)
                throw std::runtime_error(native + ": speed initialization must precede zombie_think");
        if (ref.qualifier || At(tokens, static_cast<long long>(ref.index) - 1).Low() != "self")
            throw std::runtime_error(native + ": expected synchronous zombie method speed initialization");
        const auto& movement = definition["native_movement"];
        const auto moving = Parse(files::ReadText(bo2Root / "raw" / files::FromUtf8(Replace(S(movement["script"]), "\\", "/") + ".gsc")), S(movement["script"]));
        const Function* moveFn = moving.Find(S(movement["function"]));
        const Words callback{"self", "[[", "self", ".", S(movement["callback"]), "]]", "("};
        bool invokes = false;
        if (moveFn)
        {
            const auto words = LowsRange(moving.tokens, static_cast<long long>(moveFn->bodyOpen), static_cast<long long>(moveFn->bodyClose));
            for (long long i = 0; i < static_cast<long long>(words.size()) && !invokes; i++)
                invokes = SliceIs(words, i, i + static_cast<long long>(callback.size()), callback);
        }
        if (!invokes)
            throw std::runtime_error(S(movement["script"]) + ": native movement does not invoke the source speed callback");
        tokens[ref.index].text = std::string(RUNTIME) + "::select_speed";
        tokens[fn->bodyOpen].text += "\n    self " + std::string(RUNTIME) + "::bind_spawn_location();";
        std::vector<Ref> selections;
        for (const auto& r : References(tokens, location->bodyOpen, location->bodyClose))
            if (Lower(r.name) == "random" && !r.pointer
                && LowsRange(tokens, static_cast<long long>(r.index) - 2, static_cast<long long>(r.index) + 5) == Words{"spot", "=", "random", "(", "spots", ")", ";"})
                selections.push_back(r);
        if (selections.size() != 1)
            throw std::runtime_error(native + ": expected one ordinary spawn location selection");
        tokens[selections[0].index - 2].text = "if ( isdefined( self.spawn_point_override ) )\n        spot = self.spawn_point_override;\n    else\n        spot";
        const auto words = LowsRange(tokens, 0, static_cast<long long>(tokens.size()));
        const Words copyWords{"if", "(", "isdefined", "(", "spot", ".", "target", ")", ")", "self", ".", "target", "=", "spot", ".", "target", ";"};
        std::vector<long long> copies;
        for (long long i = static_cast<long long>(location->bodyOpen); i < static_cast<long long>(location->bodyClose); i++)
            if (SliceIs(words, i, i + static_cast<long long>(copyWords.size()), copyWords))
                copies.push_back(i);
        if (copies.size() != 1)
            throw std::runtime_error(native + ": expected one spawn location target copy");
        for (long long k = copies[0]; k < copies[0] + 9 && k < static_cast<long long>(tokens.size()); k++)
            tokens[k].text = "";
        for (const auto& r : References(tokens))
            if (r.qualifier && *r.qualifier == native)
                tokens[*r.qualIndex].text = owned;
        static const pyre::Regex include(R"(#include\s+([\w\\/]+)\s*;?)", pyre::IGNORECASE);
        for (auto& token : tokens)
            if (token.kind == Kind::Directive)
                if (const auto m = include.MatchAt(token.text); m && (*m)[1] == native)
                    token.text = "#include " + owned + ";";
        auto text = WawDropRules(Emit(tokens), native);
        if (hasCore)
            text = WawQuoteRules(text, native, files::ReadText(core));
        text = Replace(text, "maps\\mp\\zombies\\_zm_score::player_add_points(", "maps\\mp\\waw\\_waw2bo2_compat::waw_player_add_points(");
        files::WriteText(outRoot / files::FromUtf8(Replace(owned, "\\", "/") + ".gsc"), "// waw2bo2: native AI with source spawn/speed adapters.\n" + text);
        const auto bootstrap = outRoot / "maps/mp/waw/_waw2bo2_zm.gsc";
        if (!fs::is_regular_file(bootstrap, error))
            return;
        auto zm = Parse(files::ReadText(bootstrap), "owned zombie bootstrap");
        auto& zt = zm.tokens;
        const std::string utility = "maps\\mp\\zombies\\_zm_utility";
        for (const auto& r : References(zt))
        {
            if (r.qualifier && *r.qualifier == native)
                zt[*r.qualIndex].text = owned;
            if (Lower(r.name) == "set_zombie_run_cycle" && (!r.qualifier || *r.qualifier == utility))
            {
                zt[r.index].text = std::string(RUNTIME) + "::refresh_speed";
                if (r.qualIndex)
                {
                    zt[*r.qualIndex].text = "";
                    At(zt, static_cast<long long>(r.index) - 1).text = "";
                }
            }
        }
        if (const Function* rs = zm.Find("round_spawning"))
        {
            const auto open = static_cast<long long>(rs->bodyOpen), close = static_cast<long long>(rs->bodyClose);
            for (long long i = open + 1; i < close - 6; i++)
                if (Lows(zt, i, 7) == Words{"level", ".", "zombie_spawn_locations", ".", "size", "<=", "0"})
                {
                    zt[i].text = "!" + std::string(RUNTIME) + "::spawn_locations_available()";
                    for (long long k = i + 1; k < i + 7; k++)
                        zt[k].text = "";
                }
            std::vector<long long> history;
            for (long long i = open + 1; i < close - 3; i++)
                if (Lows(zt, i, 4) == Words{"old_spawn", "=", "spawn_point", ";"})
                    history.push_back(i);
            if (!history.empty())
                zt[history.back() + 3].text += "\n        if ( isdefined( level.waw_source_spawn_membership ) && level.waw_source_spawn_membership )\n            spawn_point = "
                                               + std::string(RUNTIME)
                                               + "::choose_spawn_location();\n        if ( !isdefined( spawn_point ) )\n        {\n            wait 0.1;\n            continue;\n        }\n";
            for (const auto& r : References(zt, rs->bodyOpen, rs->bodyClose))
            {
                if (Lower(r.name) == "spawn_zombie" && !r.pointer && (!r.qualifier || *r.qualifier == utility))
                {
                    zt[r.index].text = std::string(RUNTIME) + "::spawn_zombie";
                    if (r.qualIndex)
                    {
                        zt[*r.qualIndex].text = "";
                        At(zt, static_cast<long long>(r.index) - 1).text = "";
                    }
                }
                if (Lower(r.name) == "random" && !r.pointer)
                {
                    const auto next = LowsRange(zt, static_cast<long long>(r.index) + 2, static_cast<long long>(r.index) + 5);
                    if (next == Words{"level", ".", "zombie_spawners"} || next == Words{"level", ".", "zombie_spawn"})
                    {
                        zt[r.index].text = std::string(RUNTIME) + "::choose_spawner";
                        zt[Match(zt, r.index + 1, "(", ")")].text = ", spawn_point )";
                    }
                }
            }
            files::WriteText(bootstrap, Emit(zt));
        }
    }
} // namespace w2b::gsc::port::zombiebehavior

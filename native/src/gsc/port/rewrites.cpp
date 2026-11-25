// The module-level token rewrites (see rewrites.h).
#include "rewrites.h"

#include "api.h"
#include "core/unicode.h"

#include <cmath>
#include <cstdio>
#include <regex>

namespace w2b::gsc::port
{
    namespace
    {
        const std::set<std::string> CALL_LIKE{"if", "while", "for", "foreach", "switch", "return", "wait", "waittill", "waittillmatch",
                                              "notify", "endon", "thread", "case", "else", "do"};
        const std::set<std::string> CONDITION_KEYWORDS{"if", "while", "for", "foreach", "switch"};

        long long Len(const Tokens& tokens)
        {
            return static_cast<long long>(tokens.size());
        }

        void Blank(Tokens& tokens, const long long a, const long long b)
        {
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            for (size_t k = lo; k < hi; k++)
                tokens[k].text = tokens[k].pre = "";
        }

        // "text in '(['" (a substring test)
        bool InChars(const std::string& text, const char* chars)
        {
            return !text.empty() && std::string_view(chars).find(text) != std::string_view::npos;
        }

        // LINE_COMMENT_BACKSLASH.sub(r"\1", text):  (//[^\n]*?)\\+[ \t]*(?=\r?\n|$)
        std::string StripCommentBackslashes(const std::string& text)
        {
            std::string out;
            const size_t n = text.size();
            size_t pos = 0;
            while (pos < n)
            {
                if (text.compare(pos, 2, "//") == 0)
                {
                    bool matched = false;
                    for (size_t k = pos + 2; k <= n && (k == pos + 2 || text[k - 1] != '\n'); k++)
                    {
                        if (k < n && text[k] == '\n')
                            break;
                        if (k >= n || text[k] != '\\')
                            continue;
                        size_t j = k;
                        while (j < n && text[j] == '\\')
                            j++;
                        size_t after = j;
                        while (after < n && (text[after] == ' ' || text[after] == '\t'))
                            after++;
                        const bool ends = after == n || text[after] == '\n' || (text[after] == '\r' && after + 1 < n && text[after + 1] == '\n')
                                          || (after + 1 == n && text[after] == '\n');
                        if (ends)
                        {
                            out.append(text, pos, k - pos);
                            pos = after;
                            matched = true;
                            break;
                        }
                    }
                    if (matched)
                        continue;
                }
                out += text[pos++];
            }
            return out;
        }
    } // namespace

    bool SourceDrives(const std::string& core, const std::string& name)
    {
        const auto& api = Api();
        if (!api.contains("source_driven") || !api["source_driven"].contains(core))
            return false;
        const auto& driven = api["source_driven"][core];
        if (driven.contains("glue") && driven["glue"].contains(name))
            return false;
        if (!driven.contains("only"))
            return true;
        for (const auto& only : driven["only"])
            if (only == name)
                return true;
        return false;
    }

    std::string PortedPath(const std::string& path)
    {
        const auto p = Norm(path);
        if (StartsWith(p, "clientscripts\\"))
            return "clientscripts\\mp\\waw\\" + p.substr(14);
        return "maps\\mp\\waw\\" + (StartsWith(p, "maps\\") ? p.substr(5) : p);
    }

    std::string AnimationTreeName(const std::string& tree)
    {
        return "waw2bo2_" + Replace(Norm(tree), "\\", "_");
    }

    std::string AnimationAssetName(const std::string& name)
    {
        return "waw_anim_" + Lower(name);
    }

    std::set<std::string> AnimationNames(const Tokens& tokens)
    {
        std::set<std::string> names;
        for (long long i = 1; i < Len(tokens) - 1; i++)
            if (At(tokens, i).text == "%" && At(tokens, i + 1).kind == Kind::Ident
                && In(At(tokens, i - 1).text, {"(", ",", "=", "[", "return", "[["}))
                names.insert(At(tokens, i + 1).Low());
        return names;
    }

    void NeutralizeAnimations(Tokens& tokens, const std::string& tree)
    {
        for (long long i = 0; i < Len(tokens); i++)
        {
            auto& t = At(tokens, i);
            if (t.kind == Kind::Directive && StartsWith(Lower(t.text), "#using_animtree"))
                t.text = "// waw2bo2: " + t.text + " (animtree not converted)";
            else if (t.text == "#" && At(tokens, i + 1).Low() == "animtree")
            {
                t.text = "";
                At(tokens, i + 1).text = "\"" + tree + "\"";
                At(tokens, i + 1).kind = Kind::String;
            }
            else if (t.text == "%" && At(tokens, i + 1).kind == Kind::Ident && In(At(tokens, i - 1).text, {"(", ",", "=", "[", "return", "[["}))
            {
                t.text = "";
                At(tokens, i + 1).text = "\"" + At(tokens, i + 1).text + "\"";
                At(tokens, i + 1).kind = Kind::String;
            }
        }
    }

    int BridgeContactKillAttacks(Tokens& tokens, const Script& script)
    {
        static const std::regex distance(R"(distance\(self\.origin,[a-z_]\w*\[[^\]]+\]\.origin\)<=\d)");
        static const std::regex start(R"(self(?:\S*::waw_)?startragdoll\(\))");
        static const std::regex launch(R"(self(?:\S*::waw_)?launchragdoll\()");
        static const std::regex damage(R"(self(?:\S*::waw_)?dodamage\(self\.health\+\d)");
        int fixes = 0;
        for (const auto& [_, fn] : script.functions)
        {
            const auto a = static_cast<long long>(fn.bodyOpen) + 1, b = static_cast<long long>(fn.bodyClose);
            const auto code = JoinLow(tokens, a, b);
            if (!(Contains(code, "getplayers()") && Contains(code, "isalive(self)") && std::regex_search(code, distance)
                  && std::regex_search(code, start) && std::regex_search(code, launch) && std::regex_search(code, damage) && Contains(code, "wait")))
                continue;
            const auto [lo, hi] = Slice(tokens.size(), a, b);
            bool returnsValue = false;
            for (size_t k = lo; k + 1 < hi; k++)
                if (tokens[k].Low() == "return" && tokens[k + 1].text != ";")
                    returnsValue = true;
            if (returnsValue)
                continue;
            const std::string begin = std::string("\n    self ") + COMPAT + "::waw_contact_kill_begin();";
            const std::string end = std::string("self ") + COMPAT + "::waw_contact_kill_end();";
            tokens[fn.bodyOpen].text += begin;
            for (size_t k = lo; k < hi; k++)
                if (tokens[k].Low() == "return")
                {
                    tokens[k].text = "{ " + end + " return";
                    if (k + 1 >= hi)
                        throw std::out_of_range("list index out of range");
                    tokens[k + 1].text = "; }";
                }
            tokens[fn.bodyClose].text = end + "\n}";
            fixes++;
        }
        return fixes;
    }

    int FixSyntax(Tokens& tokens)
    {
        int fixes = 0;
        for (auto& t : tokens)
        {
            auto [repaired, count] = RepairBlockComments(t.pre);
            t.pre = std::move(repaired);
            fixes += count;
            if (Contains(t.pre, "\\"))
            {
                auto fixed = StripCommentBackslashes(t.pre);
                fixes += fixed != t.pre;
                t.pre = std::move(fixed);
            }
            if (t.kind == Kind::Directive)
            {
                auto fixed = RStripChars(t.pre, " \t");
                fixes += fixed != t.pre;
                t.pre = std::move(fixed);
            }
        }
        for (long long i = 0; i < Len(tokens); i++)
        {
            auto& t = At(tokens, i);
            if (t.text != "(" || t.kind != Kind::Punct)
                continue;
            const Token* prev = i ? &At(tokens, i - 1) : nullptr;
            if (prev && ((prev->kind == Kind::Ident && !CALL_LIKE.count(prev->Low())) || In(prev->text, {")", "]", "]]", "::"})))
                continue;
            int depth = 0;
            bool simple = true;
            long long j = i;
            bool broke = false;
            for (; j < Len(tokens); j++)
            {
                const auto& u = At(tokens, j);
                if (u.text == "(" || u.text == "[")
                {
                    depth++;
                    if (u.text == "(" && j > i)
                        simple = false;
                }
                else if (u.text == ")" || u.text == "]")
                {
                    depth--;
                    if (depth == 0)
                    {
                        broke = true;
                        break;
                    }
                }
                else if (depth == 1 && !(u.kind == Kind::Ident || u.text == "."))
                    simple = false;
            }
            if (!broke)
                continue;
            const Token* after = j + 1 < Len(tokens) ? &At(tokens, j + 1) : nullptr;
            const bool methodCall = after && after->kind == Kind::Ident && j + 2 < Len(tokens)
                                    && (At(tokens, j + 2).text == "(" || In(after->Low(), {"thread", "childthread"}))
                                    && !(prev && CONDITION_KEYWORDS.count(prev->Low()));
            if (simple && j > i + 1 && At(tokens, j).text == ")" && after && (after->text == "." || methodCall))
            {
                t.text = "";
                At(tokens, j).text = "";
                fixes++;
            }
        }
        return fixes + SplitCallReceivers(tokens);
    }

    int SplitCallReceivers(Tokens& tokens)
    {
        int count = 0;
        long long i = 1;
        while (i < Len(tokens) - 2)
        {
            const Token t = At(tokens, i);
            const Token nxt = At(tokens, i + 1);
            const Token after = At(tokens, i + 2);
            if (!(t.text == ")" && t.kind == Kind::Punct && nxt.kind == Kind::Ident
                  && ((!CALL_LIKE.count(nxt.Low()) && after.text == "(") || In(nxt.Low(), {"thread", "childthread"}))))
            {
                i++;
                continue;
            }
            int depth = 0;
            long long j = i;
            while (j > 0)
            {
                const auto& text = At(tokens, j).text;
                depth += text == ")" ? 1 : text == "(" ? -1 : 0;
                if (depth == 0)
                    break;
                j--;
            }
            const Token* callee = j > 0 ? &At(tokens, j - 1) : nullptr;
            const Token* start = j > 1 ? &At(tokens, j - 2) : nullptr;
            if (!callee || callee->kind != Kind::Ident || CALL_LIKE.count(callee->Low()) || !start || !In(start->text, {";", "{", "}"}))
            {
                i++;
                continue;
            }
            const Token calleeCopy = *callee;
            tokens.insert(tokens.begin() + (j - 1), {Token{Kind::Ident, "waw_receiver", calleeCopy.pre, calleeCopy.line},
                                                     Token{Kind::Punct, "=", " ", calleeCopy.line}});
            At(tokens, j + 1).pre = " "; // the callee, shifted by the two inserted tokens
            i += 2;
            tokens.insert(tokens.begin() + (i + 1), {Token{Kind::Punct, ";", "", t.line}, Token{Kind::Ident, "waw_receiver", nxt.pre, nxt.line}});
            At(tokens, i + 3).pre = " "; // nxt
            count++;
            i += 3;
        }
        return count;
    }

    const std::map<std::string, std::string>& WawLevelFields()
    {
        static const std::map<std::string, std::string> fields = []
        {
            std::map<std::string, std::string> f{{"script", "waw_script"}, {"chests", "waw_chests"}, {"chest_index", "waw_chest_index"}, {"_effect", "waw_effect"}};
            f["zombie_move_speed"] = Api()["zombie_runtime"]["source_speed"]["owned_result_field"].get<std::string>();
            for (const char* name : {"zombie_weapons", "zombie_include_weapons", "limited_weapons", "weapon_weighting_funcs", "zombie_treasure_chest_cost",
                                     "chest_accessed", "chest_min_move_usage", "open_chest_location", "verify_chest", "random_pandora_box_start",
                                     "pulls_since_last_ray_gun", "pulls_since_last_tesla_gun", "player_drops_tesla_gun", "player_seen_tesla_gun",
                                     "overrideplayerdamage", "playerlaststand_func", "player_becomes_zombie", "laststandpistol", "global_damage_func",
                                     "global_damage_func_ads"})
                f[name] = std::string("waw_") + name;
            f["zone_manager_init_func"] = "waw_zone_manager_init_func";
            f["enemy_dog_locations"] = "waw_enemy_dog_locations";
            f["enemy_dog_spawns"] = "waw_enemy_dog_spawns";
            return f;
        }();
        return fields;
    }

    int RenameLevelFields(Tokens& tokens)
    {
        static const std::set<std::string> boxFlags{"moving_chest_enabled", "moving_chest_now", "chest_has_been_used", "weapon_upgrade"};
        const auto& api = Api();
        const auto owned = api["zombie_runtime"]["source_speed"]["owned_result_field"].get<std::string>();
        bool chasePreserved = false;
        if (api.contains("preserved_scripts"))
            for (const auto& s : api["preserved_scripts"])
                if (s == api["zombie_runtime"]["source_chase"]["script"])
                    chasePreserved = true;
        const auto& fields = WawLevelFields();
        int renamed = 0;
        for (long long i = 2; i < Len(tokens); i++)
        {
            auto& t = At(tokens, i);
            if (t.kind == Kind::String && StartsWith(t.text, "\"") && t.text.size() >= 2 && boxFlags.count(t.text.substr(1, t.text.size() - 2)))
            {
                t.text = "\"waw_" + t.text.substr(1, t.text.size() - 2) + "\"";
                renamed++;
            }
            const auto low = t.Low();
            if (t.kind == Kind::Ident && fields.count(low) && At(tokens, i - 1).text == "." && At(tokens, i - 2).Low() == "level"
                && (i + 1 >= Len(tokens) || At(tokens, i + 1).text != "("))
            {
                t.text = fields.at(low);
                renamed++;
            }
            else if (t.kind == Kind::Ident && low == "zombie_move_speed" && At(tokens, i - 1).text == "." && !chasePreserved)
            {
                t.text = owned;
                renamed++;
            }
        }
        return renamed;
    }

    std::set<std::string> LevelFieldsRead(const Tokens& tokens, const long long start, const std::optional<long long> endOpt)
    {
        const long long end = endOpt.value_or(Len(tokens));
        std::set<std::string> out;
        for (long long i = std::max(start, 2LL); i < end; i++)
            if (At(tokens, i).kind == Kind::Ident && At(tokens, i - 1).text == "." && At(tokens, i - 2).Low() == "level")
                out.insert(At(tokens, i).Low());
        return out;
    }

    std::set<std::string> LevelFieldsAssigned(const Tokens& tokens, const long long start, const std::optional<long long> endOpt)
    {
        const long long end = endOpt.value_or(Len(tokens));
        std::set<std::string> out;
        for (long long i = std::max(start, 2LL); i < std::min(end, Len(tokens) - 1); i++)
            if (At(tokens, i).kind == Kind::Ident && At(tokens, i - 1).text == "." && At(tokens, i - 2).Low() == "level" && At(tokens, i + 1).text == "=")
                out.insert(At(tokens, i).Low());
        return out;
    }

    std::optional<std::string> GuardedLevelDefault(const Tokens& tokens)
    {
        std::vector<std::string> words;
        for (const auto& t : tokens)
            words.push_back(t.Low());
        const std::vector<std::string> head{"if", "(", "!", "isdefined", "(", "level", "."};
        if (words.size() < 16 || !std::equal(head.begin(), head.end(), words.begin()))
            return std::nullopt;
        const auto field = words[7];
        if (words[8] != ")" || words[9] != ")")
            return std::nullopt;
        std::vector<std::string> body(words.begin() + 10, words.end());
        if (body.front() == "{" && body.back() == "}")
            body = std::vector<std::string>(body.begin() + 1, body.end() - 1);
        if (body.size() < 4 || body[0] != "level" || body[1] != "." || body[2] != field || body[3] != "=" || body.back() != ";"
            || std::count(body.begin(), body.end(), ";") != 1)
            return std::nullopt;
        return field;
    }

    int BridgePoiRecordComparisons(Tokens& tokens)
    {
        static const std::set<std::string> fields{"attractor_positions", "claimed_attractor_positions"};
        int count = 0;
        for (const auto& ref : References(tokens))
        {
            const auto idx = static_cast<long long>(ref.index);
            auto low = At(tokens, idx).Low();
            const auto last = low.substr(low.rfind("::") == std::string::npos ? 0 : low.rfind("::") + 2);
            if (ref.pointer || !In(last, {"array_check_for_dupes", "array_remove", "waw_array_remove"}))
                continue;
            const auto start = idx + 2;
            if (!(At(tokens, start).kind == Kind::Ident && At(tokens, start + 1).text == "." && fields.count(At(tokens, start + 2).Low())
                  && At(tokens, start + 3).text == ","))
                continue;
            if (ref.qualIndex)
            {
                At(tokens, static_cast<long long>(*ref.qualIndex)).text = "";
                At(tokens, static_cast<long long>(*ref.qualIndex) + 1).text = "";
            }
            const std::string helper = last == "array_check_for_dupes" ? "waw_poi_record_available" : "waw_poi_record_remove";
            At(tokens, idx).text = std::string(COMPAT) + "::" + helper;
            count++;
        }
        for (long long i = 0; i < Len(tokens) - 6; i++)
        {
            if (!fields.count(At(tokens, i).Low()) || At(tokens, i - 1).text != "." || At(tokens, i + 1).text != "[")
                continue;
            const auto end = static_cast<long long>(Match(tokens, static_cast<size_t>(i + 1), "[", "]"));
            if (At(tokens, end + 1).text == "==" && At(tokens, end + 2).kind == Kind::Ident && At(tokens, end + 3).text == ")")
            {
                At(tokens, end).text += "[0]";
                At(tokens, end + 2).text += "[0]";
                count++;
            }
        }
        return count;
    }

    int BridgeReviveReads(Tokens& tokens)
    {
        int count = 0;
        for (long long i = 0; i < Len(tokens); i++)
        {
            if (At(tokens, i).Low() != "being_revived" || At(tokens, i - 1).text != ".")
                continue;
            if (In(At(tokens, i + 1).text, {"=", "+=", "-=", "++", "--"}))
                continue;
            const auto j = PathStart(tokens, i - 1);
            if (j >= 2 && At(tokens, j - 1).text == "(" && At(tokens, j - 2).Low() == "isdefined")
                continue;
            const auto receiver = PyLStrip(EmitRange(tokens, j, i - 1));
            At(tokens, j).text = std::string(COMPAT) + "::waw_being_revived( " + receiver + " )";
            At(tokens, j).kind = Kind::Punct;
            Blank(tokens, j + 1, i + 1);
            count++;
        }
        return count;
    }

    int BridgeDrinkingFlag(Tokens& tokens)
    {
        int count = 0;
        for (long long i = 2; i < Len(tokens) - 2; i++)
        {
            if (At(tokens, i).Low() != "is_drinking" || At(tokens, i - 1).text != ".")
                continue;
            if (At(tokens, i + 1).text == "=" && At(tokens, i + 2).Low() == "undefined")
            {
                At(tokens, i + 2).text = "0";
                count++;
                continue;
            }
            const auto j = PathStart(tokens, i - 1);
            if (!(j >= 2 && At(tokens, j - 1).text == "(" && At(tokens, j - 2).Low() == "isdefined" && At(tokens, i + 1).text == ")"))
                continue;
            const auto receiver = PyLStrip(EmitRange(tokens, j, i - 1));
            At(tokens, j - 2).text = std::string(COMPAT) + "::waw_is_drinking( " + receiver + " )";
            At(tokens, j - 2).kind = Kind::Punct;
            Blank(tokens, j - 1, i + 2);
            count++;
        }
        return count;
    }

    int BridgeScoreWrites(Tokens& tokens)
    {
        int count = 0;
        for (long long i = 2; i < Len(tokens) - 2; i++)
        {
            if (At(tokens, i).Low() != "score" || At(tokens, i - 1).text != "." || !In(At(tokens, i + 1).text, {"=", "+=", "-=", "*=", "/=", "++", "--"}))
                continue;
            const auto j = PathStart(tokens, i - 1);
            const auto receiver = PyStrip(EmitRange(tokens, j, i - 1));
            if (receiver.empty() || Contains(receiver, "("))
                continue;
            long long end = i + 1;
            int depth = 0;
            while (end < Len(tokens) && !(At(tokens, end).text == ";" && depth == 0))
            {
                depth += In(At(tokens, end).text, {"(", "["});
                depth -= In(At(tokens, end).text, {")", "]"});
                end++;
            }
            if (end >= Len(tokens))
                continue;
            const bool braceless = At(tokens, j - 1).text == ")" || At(tokens, j - 1).Low() == "else";
            if (braceless)
                At(tokens, j).pre += "{ ";
            At(tokens, end).text = "; " + receiver + ".pers[\"score\"] = " + receiver + ".score;" + (braceless ? " }" : "");
            count++;
        }
        return count;
    }

    int BridgePlayerStatReads(Tokens& tokens)
    {
        static const std::set<std::string> keys{"kills", "score", "downs", "revives", "perks", "headshots", "zombie_gibs"};
        int count = 0;
        for (long long i = 2; i < Len(tokens) - 4; i++)
        {
            if (At(tokens, i).Low() != "stats" || At(tokens, i - 1).text != "." || At(tokens, i + 1).text != "[" || At(tokens, i + 2).kind != Kind::String
                || !keys.count(StripChars(At(tokens, i + 2).text, "\"")) || At(tokens, i + 3).text != "]"
                || In(At(tokens, i + 4).text, {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "++", "--"}))
                continue;
            const auto j = PathStart(tokens, i - 1);
            if (j && In(At(tokens, j - 1).text, {"++", "--"}))
                continue;
            if (j >= 2 && At(tokens, j - 1).text == "(" && At(tokens, j - 2).Low() == "isdefined")
                continue;
            const auto receiver = PyLStrip(EmitRange(tokens, j, i - 1));
            At(tokens, j).text = std::string(COMPAT) + "::waw_player_stat( " + receiver + ", " + At(tokens, i + 2).text + " )";
            At(tokens, j).kind = Kind::Punct;
            Blank(tokens, j + 1, i + 4);
            count++;
        }
        return count;
    }

    int BridgeDynamicHudMaterials(Tokens& tokens)
    {
        int rewrites = 0;
        for (long long i = 1; i < Len(tokens) - 2; i++)
        {
            if (At(tokens, i).Low() != "setshader" || At(tokens, i + 1).text != "(" || At(tokens, i - 1).text == "::")
                continue;
            const auto close = static_cast<long long>(Match(tokens, static_cast<size_t>(i + 1), "(", ")"));
            if (close == i + 2)
                continue;
            long long end = close;
            int depth = 0;
            for (long long j = i + 2; j < close; j++)
            {
                depth += In(At(tokens, j).text, {"(", "["});
                depth -= In(At(tokens, j).text, {")", "]"});
                if (At(tokens, j).text == "," && depth == 0)
                {
                    end = j;
                    break;
                }
            }
            if (end == i + 3 && At(tokens, i + 2).kind == Kind::String)
                continue;
            At(tokens, i + 2).text = std::string(COMPAT) + "::waw_hud_material( " + At(tokens, i + 2).text;
            At(tokens, end - 1).text += " )";
            rewrites++;
        }
        return rewrites;
    }

    int MirrorTeamZombieVars(Tokens& tokens)
    {
        std::set<std::string> names;
        for (const auto& n : Api()["team_zombie_vars"]["names"])
            names.insert(n.get<std::string>());
        int rewrites = 0;
        for (long long i = 0; i < Len(tokens) - 6; i++)
        {
            const auto& t = At(tokens, i);
            std::optional<std::string> name;
            if (t.Low() == "level" && At(tokens, i + 1).text == "." && At(tokens, i + 2).text == "zombie_vars" && At(tokens, i + 3).text == "["
                && At(tokens, i + 4).kind == Kind::String && At(tokens, i + 5).text == "]"
                && In(At(tokens, i + 6).text, {"=", "+=", "-=", "*=", "/=", "++", "--"}) && (i == 0 || At(tokens, i - 1).text != "."))
                name = StripChars(At(tokens, i + 4).text, "\"");
            else if (EndsWith(t.Low(), "set_zombie_var") && At(tokens, i + 1).text == "(" && At(tokens, i + 2).kind == Kind::String)
                name = StripChars(At(tokens, i + 2).text, "\"");
            if (!name || !names.count(*name))
                continue;
            int depth = 0;
            for (long long j = i; j < Len(tokens); j++)
            {
                depth += In(At(tokens, j).text, {"(", "["});
                depth -= In(At(tokens, j).text, {")", "]"});
                if (depth < 0 || In(At(tokens, j).text, {"{", "}"}))
                    break;
                if (At(tokens, j).text == ";" && depth == 0)
                {
                    At(tokens, j).text += std::string(" ") + COMPAT + "::waw_team_zombie_var( \"" + *name + "\" );";
                    rewrites++;
                    break;
                }
            }
        }
        return rewrites;
    }

    std::string HudMaterialTableSource(const std::map<std::string, std::string>& renamed)
    {
        std::vector<std::string> lines{"// waw2bo2: converted HUD materials renamed out of BO2's names (materialisolation).", "table()", "{",
                                       "\tmaterials = [];"};
        for (const auto& [old, replacement] : renamed)
        {
            const auto s = unicode::Decode(old);
            const auto head = unicode::Encode(s.substr(0, 1));
            const auto tail = unicode::Encode(s.size() > 1 ? s.substr(1) : std::u32string());
            lines.push_back("\tmaterials[\"" + head + "\" + \"" + tail + "\"] = \"" + replacement + "\";");
        }
        lines.insert(lines.end(), {"\treturn materials;", "}", ""});
        return Join(lines, "\n");
    }

    int BridgeFontScales(Tokens& tokens)
    {
        int changed = 0;
        for (long long i = 0; i < Len(tokens); i++)
        {
            if (At(tokens, i).Low() != "fontscale" || i < 1 || At(tokens, i - 1).text != "." || At(tokens, i + 1).text != "=")
                continue;
            const long long start = i + 2;
            long long end = i + 2;
            int depth = 0;
            while (end < Len(tokens))
            {
                if (At(tokens, end).text == ";" && !depth)
                    break;
                depth += In(At(tokens, end).text, {"(", "["});
                depth -= In(At(tokens, end).text, {")", "]"});
                end++;
            }
            if (end == Len(tokens))
                continue;
            const auto rhs = PyStrip(EmitRange(tokens, start, end));
            std::string replacement;
            if (end == start + 1 && At(tokens, start).kind == Kind::Number)
            {
                double scale = 0;
                util::ParseFloat(At(tokens, start).text, scale);
                const auto steps = static_cast<long long>(std::floor((scale - 1) * 10 + 0.5));
                const double decoded = 1 + static_cast<double>(steps & 63) / 10;
                if (std::abs(decoded - scale) < 1e-6)
                    continue;
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%g", decoded);
                replacement = buffer;
            }
            else
                replacement = std::string(COMPAT) + "::waw_fontscale( " + rhs + " )";
            At(tokens, start).text = replacement;
            At(tokens, start).kind = Kind::Punct;
            Blank(tokens, start + 1, end);
            changed++;
        }
        return changed;
    }

    bool BridgeHudCleanup(Tokens& tokens, const long long index, const std::string& wrapper)
    {
        if (index < 1 || At(tokens, index - 1).kind != Kind::Ident)
            return false;
        long long start = index - 1;
        while (start >= 2 && At(tokens, start - 1).text == "." && At(tokens, start - 2).kind == Kind::Ident)
            start -= 2;
        if (start && In(At(tokens, start - 1).text, {".", "]", ")", "thread"}))
            return false;
        const auto receiver = PyStrip(EmitRange(tokens, start, index));
        At(tokens, start).text = std::string(COMPAT) + "::" + wrapper + "( " + receiver + " )";
        At(tokens, start).kind = Kind::Punct;
        Blank(tokens, start + 1, index + 3);
        return true;
    }

    int KeepBo2Deathanims(Tokens& tokens)
    {
        int count = 0;
        for (long long i = 0; i < Len(tokens); i++)
        {
            const auto& t = At(tokens, i);
            if (t.Low() != "deathanim" || t.kind != Kind::Ident || At(tokens, i - 1).text != "." || At(tokens, i + 1).text != "=")
                continue;
            const auto j = PathStart(tokens, i - 1);
            const auto target = JoinText(tokens, j, i + 1);
            long long k = i + 2;
            int depth = 0;
            while (At(tokens, k).kind != Kind::Eof && (At(tokens, k).text != ";" || depth))
            {
                depth += In(At(tokens, k).text, {"(", "[", "{"});
                depth -= In(At(tokens, k).text, {")", "]", "}"});
                k++;
            }
            At(tokens, i + 2).text = target;
            At(tokens, i + 2).pre = " ";
            At(tokens, i + 2).kind = Kind::Punct;
            for (long long m = i + 3; m < k + 1; m++)
            {
                At(tokens, m).pre = "";
                if (m < k)
                {
                    At(tokens, m).text = "";
                    At(tokens, m).kind = Kind::Punct;
                }
            }
            count++;
        }
        return count;
    }

    bool UsesAnimations(const Tokens& tokens, const long long start, const long long end)
    {
        for (long long i = start + 1; i < end; i++)
            if (At(tokens, i).text == "%" && At(tokens, i + 1).kind == Kind::Ident && In(At(tokens, i - 1).text, {"(", ",", "=", "[", "return", "[["}))
                return true;
        return false;
    }

    void TruncateArgs(Tokens& tokens, const long long nameIndex, const int keep)
    {
        int depth = 0;
        int count = 0;
        for (long long j = nameIndex + 1; j < Len(tokens); j++)
        {
            auto& t = At(tokens, j);
            if (t.kind == Kind::Punct && In(t.text, {"(", "[", "{", "[["}))
            {
                depth++;
                if (depth == 1)
                    continue;
            }
            else if (t.kind == Kind::Punct && In(t.text, {")", "]", "}", "]]"}))
            {
                depth--;
                if (depth == 0)
                    return;
            }
            else if (depth == 1 && t.text == ",")
                count++;
            if (depth >= 1 && (keep ? count >= keep : true) && !(keep && count == keep - 1))
                if (count >= keep)
                {
                    t.text = t.pre = "";
                    t.kind = Kind::Punct;
                }
        }
    }

    std::optional<int> Argc(const Tokens& tokens, const long long nameIndex)
    {
        const long long i = nameIndex + 1;
        if (At(tokens, i).text != "(")
            return std::nullopt;
        int depth = 0, count = 0;
        bool empty = true;
        for (long long j = i; j < Len(tokens); j++)
        {
            const auto& t = At(tokens, j);
            if (t.kind == Kind::Punct && In(t.text, {"(", "[", "{", "[["}))
                depth++;
            else if (t.kind == Kind::Punct && In(t.text, {")", "]", "}", "]]"}))
            {
                depth--;
                if (depth == 0)
                    return empty ? 0 : count + 1;
            }
            else if (depth == 1 && t.text == ",")
                count++;
            else if (depth >= 1)
                empty = false;
        }
        return std::nullopt;
    }

    int BridgeIntermissionCamera(Tokens& tokens)
    {
        const auto linkto = Lower(std::string(COMPAT) + "::waw_linkto");
        int count = 0;
        for (long long i = 1; i < Len(tokens) - 4; i++)
        {
            if (!In(At(tokens, i).Low(), {"linkto", linkto}) || At(tokens, i - 1).Low() != "self" || At(tokens, i + 1).text != "(")
                continue;
            const auto close = static_cast<long long>(Match(tokens, static_cast<size_t>(i + 1), "(", ")"));
            const auto& mover = At(tokens, i + 2);
            if (mover.kind != Kind::Ident || !In(At(tokens, i + 3).text, {",", ")"}) || At(tokens, close + 1).text != ";")
                continue;
            const auto& m = mover.text;
            At(tokens, close + 1).text += "\n\tif ( !isdefined( self.waw_intermission_cam ) )\n\t{"
                                          "\n\t\tself.waw_intermission_cam = spawn( \"script_model\", "
                                          + m + ".origin );\n\t\tself.waw_intermission_cam setmodel( \"tag_origin\" );\n\t\tself.waw_intermission_cam.angles = " + m
                                          + ".angles;\n\t\tself.waw_intermission_cam linkto( " + m
                                          + " );\n\t\tself camerasetposition( self.waw_intermission_cam );\n\t\tself camerasetlookat();\n\t\tself cameraactivate( 1 );\n\t}";
            count++;
        }
        return count;
    }

    int BridgeLaunchTriggers(Tokens& tokens)
    {
        const auto script = Parse(Emit(tokens), "launch-translation");
        int fixes = 0;
        for (const auto& [_, fn] : script.functions)
        {
            const auto start = static_cast<long long>(fn.bodyOpen), end = static_cast<long long>(fn.bodyClose);
            std::map<std::string, std::string> roots;
            // the indices come from the reparsed text: tokens emptied by earlier
            // rewrites are gone there, so the slices can run past the list
            // (Python's slices stop at its end)
            const auto lows = [&](const long long a, const long long n)
            {
                std::vector<std::string> words;
                const auto [lo, hi] = Slice(tokens.size(), a, a + n);
                for (size_t k = lo; k < hi; k++)
                    words.push_back(tokens[k].Low());
                return words;
            };
            const auto is = [](const std::vector<std::string>& words, const size_t a, const std::vector<std::string>& expected)
            {
                if (words.size() < a + expected.size())
                    return false;
                return std::equal(expected.begin(), expected.end(), words.begin() + static_cast<long long>(a));
            };
            const auto word = [](const std::vector<std::string>& words, const size_t k) -> const std::string&
            {
                if (k >= words.size())
                    throw std::out_of_range("list index out of range");
                return words[k];
            };
            for (long long i = start; i < end - 9; i++)
            {
                const auto words = lows(i, 10);
                if (is(words, 1, {"=", "getent", "("}) && is(words, 5, {".", "target", ","}) && Lower(At(tokens, i + 8).text) == "\"targetname\""
                    && word(words, 9) == ")" && std::find(fn.params.begin(), fn.params.end(), words[4]) != fn.params.end())
                    roots[words[0]] = At(tokens, i + 4).text;
            }
            for (long long i = start; i < end - 8; i++)
            {
                const auto words = lows(i, 9);
                if (!is(words, 0, {"if", "(", "self", "istouching", "("}) || words.size() != 9 || !is(words, 6, {")", ")", "{"}) || !roots.count(words[5]))
                    continue;
                int depth = 1;
                long long j = i + 9;
                while (j < end && depth)
                {
                    depth += (At(tokens, j).text == "{") - (At(tokens, j).text == "}");
                    j++;
                }
                bool sets = false;
                for (const auto& w : lows(i + 9, j - (i + 9)))
                    sets = sets || w == "setvelocity";
                if (!sets)
                    continue;
                At(tokens, i + 2).text = "self isTouching(" + roots[words[5]] + ") || self";
                fixes++;
            }
        }
        return fixes;
    }

    int LowerMaxhealthAssignments(Tokens& tokens)
    {
        return LowerFieldAssignments(tokens, "maxhealth", "waw_set_maxhealth");
    }

    int LowerHealthAssignments(Tokens& tokens)
    {
        return LowerFieldAssignments(tokens, "health", "waw_set_health");
    }

    int LowerFieldAssignments(Tokens& tokens, const std::string& field, const std::string& helper)
    {
        int changed = 0;
        for (long long i = 2; i < Len(tokens) - 2; i++)
        {
            const auto op = At(tokens, i + 1).text;
            if (At(tokens, i).Low() != field || At(tokens, i - 1).text != "." || !In(op, {"=", "-=", "+="}))
                continue;
            long long start = i - 2;
            while (true)
            {
                if (At(tokens, start).text == "]")
                {
                    int depth = 0;
                    while (true)
                    {
                        const auto& text = At(tokens, start).text;
                        depth += text == "]" ? 1 : text == "[" ? -1 : 0;
                        if (depth == 0)
                            break;
                        start--;
                    }
                    start--;
                }
                if (At(tokens, start).kind != Kind::Ident && At(tokens, start).kind != Kind::Path && !In(At(tokens, start).Low(), {"self", "level"}))
                    break;
                if (At(tokens, start - 1).text != ".")
                    break;
                start -= 2;
            }
            const auto prevText = PyStrip(At(tokens, start - 1).text);
            const auto lastChar = prevText.empty() ? std::string() : unicode::Encode(unicode::Decode(prevText).substr(unicode::Decode(prevText).size() - 1));
            if (At(tokens, start).kind != Kind::Ident || (!In(lastChar, {";", "{", "}", ")"}) && At(tokens, start - 1).Low() != "else"))
                continue;
            long long end = i + 2;
            int depth = 0;
            while (depth || At(tokens, end).text != ";")
            {
                if (At(tokens, end).kind == Kind::Eof)
                    break;
                depth += In(At(tokens, end).text, {"(", "[", "{"});
                depth -= In(At(tokens, end).text, {")", "]", "}"});
                end++;
            }
            if (At(tokens, end).text != ";")
                continue;
            const auto receiver = PyStrip(EmitRange(tokens, start, i - 1));
            At(tokens, i - 1).text = "";
            At(tokens, i).text = std::string(" ") + COMPAT + "::" + helper + "(";
            At(tokens, i).kind = Kind::Punct;
            At(tokens, i).pre = "";
            At(tokens, i + 1).text = At(tokens, i + 1).pre = "";
            At(tokens, i + 2).pre = " ";
            if (op != "=")
            {
                At(tokens, i + 1).text = " " + receiver + "." + field + " " + op.substr(0, 1) + " (";
                At(tokens, end).text = " ) );";
            }
            else
                At(tokens, end).text = " );";
            changed++;
        }
        return changed;
    }

    int LowerHudFields(Tokens& tokens)
    {
        int changed = 0;
        for (long long i = 1; i < Len(tokens) - 3; i++)
        {
            if (At(tokens, i - 1).text != "." || At(tokens, i + 1).text != "=" || At(tokens, i + 2).text == "=")
                continue;
            const auto field = At(tokens, i).Low();
            auto& value = At(tokens, i + 2);
            if ((field == "horzalign" || field == "vertalign") && value.kind == Kind::String && At(tokens, i + 3).text == ";")
            {
                const auto inner = Lower(value.text.size() >= 2 ? value.text.substr(1, value.text.size() - 2) : std::string());
                std::string target;
                if (field == "horzalign")
                    target = inner == "left" ? "user_left" : inner == "right" ? "user_right" : "";
                else
                    target = inner == "top" ? "user_top" : inner == "bottom" ? "user_bottom" : "";
                if (!target.empty())
                {
                    value.text = "\"" + target + "\"";
                    changed++;
                }
            }
            else if (field == "hidewheninmenu" && At(tokens, i + 3).text == ";" && In(value.Low(), {"false", "0"}))
            {
                value.text = "true";
                changed++;
            }
        }
        return changed;
    }

    int LowerZombieWeaponKeys(Tokens& tokens)
    {
        const std::vector<std::string> pattern{"getarraykeys", "(", "level", ".", "zombie_weapons", ")"};
        int changed = 0;
        for (long long i = 0; i < Len(tokens) - static_cast<long long>(pattern.size()); i++)
        {
            if (!WordsEqual(tokens, i, pattern) || In(At(tokens, i - 1).text, {".", "::"}))
                continue;
            At(tokens, i).text = std::string(COMPAT) + "::waw_zombie_weapon_keys";
            At(tokens, i).kind = Kind::Punct;
            Blank(tokens, i + 2, i + 5);
            At(tokens, i + 5).pre = "";
            changed++;
        }
        return changed;
    }

    std::optional<long long> CloseParen(const Tokens& tokens, const long long start)
    {
        int depth = 0;
        for (long long i = start; i < Len(tokens); i++)
        {
            depth += (At(tokens, i).text == "(") - (At(tokens, i).text == ")");
            if (!depth)
                return i;
        }
        return std::nullopt;
    }

    int WrapIntegerLiterals(Tokens& tokens)
    {
        int changed = 0;
        for (auto& t : tokens)
        {
            if (t.kind != Kind::Number || t.text.empty() || !std::all_of(t.text.begin(), t.text.end(), [](const char c) { return c >= '0' && c <= '9'; }))
                continue;
            unsigned long long low = 0; // value & 0xFFFFFFFF of an integer of any size
            for (const char c : t.text)
                low = (low * 10 + static_cast<unsigned>(c - '0')) & 0xFFFFFFFFull;
            // value <= 0x7FFFFFFF: compare the full decimal value
            const auto digits = t.text.substr(t.text.find_first_not_of('0') == std::string::npos ? t.text.size() - 1 : t.text.find_first_not_of('0'));
            const bool fits = digits.size() < 10 || (digits.size() == 10 && digits <= "2147483647");
            if (fits)
                continue;
            long long wrapped = static_cast<long long>(low);
            if (wrapped >= 0x80000000LL)
                wrapped -= 1LL << 32;
            if (wrapped == -0x80000000LL)
                t.text = "( -2147483647 - 1 )";
            else if (wrapped < 0)
                t.text = "( " + std::to_string(wrapped) + " )";
            else
                t.text = std::to_string(wrapped);
            changed++;
        }
        return changed;
    }

    int LowerStationaryFxCarriers(Tokens& tokens)
    {
        std::vector<std::pair<long long, long long>> bodies;
        long long i = 0;
        while (i < Len(tokens))
        {
            if (At(tokens, i).text == "{")
            {
                const auto close = static_cast<long long>(Match(tokens, static_cast<size_t>(i), "{", "}"));
                if (i && At(tokens, i - 1).text == ")")
                    bodies.emplace_back(i + 1, close);
                i = close + 1;
            }
            else
                i++;
        }
        int changed = 0;
        std::vector<std::vector<std::string>> patterns;
        for (const char* p : {"self.fx = spawn(\"script_model\", self.origin);", "self.fx setmodel(\"tag_origin\");", "self.fx.angles = self.angles;",
                              "self.fx.origin = self.origin;", "self.fx linkto(self, fxTag);"})
        {
            std::vector<std::string> words;
            auto toks = Tokenize(p);
            toks.pop_back();
            for (const auto& t : toks)
                words.push_back(t.Low());
            patterns.push_back(std::move(words));
        }
        for (const auto& [begin, end] : bodies)
        {
            std::vector<std::string> words;
            for (const auto& t : tokens)
                words.push_back(t.Low());
            const auto W = [&](long long k) -> const std::string& {
                if (k < 0)
                    k += static_cast<long long>(words.size());
                return words.at(static_cast<size_t>(k));
            };
            bool excluded = false;
            bool noteworthy = false;
            for (long long k = begin; k < std::min<long long>(end, static_cast<long long>(words.size())); k++)
            {
                excluded = excluded || In(words[static_cast<size_t>(k)], {"waittill", "waittill_any", "moveto", "rotateyaw", "delete", "endon"});
                noteworthy = noteworthy || words[static_cast<size_t>(k)] == "script_noteworthy";
            }
            if (excluded)
                continue;
            bool tag = false;
            for (long long k = begin; k < end - 3; k++)
                tag = tag || (W(k) == "fxtag" && W(k + 1) == "=" && W(k + 2) == "\"tag_origin\"" && W(k + 3) == ";");
            if (!noteworthy || !tag)
                continue;
            std::vector<std::pair<long long, long long>> ranges;
            for (const auto& pattern : patterns)
            {
                std::vector<long long> hits;
                const auto n = static_cast<long long>(pattern.size());
                for (long long k = begin; k < end - n + 1; k++)
                {
                    bool same = k + n <= static_cast<long long>(words.size());
                    for (long long m = 0; same && m < n; m++)
                        same = words[static_cast<size_t>(k + m)] == pattern[static_cast<size_t>(m)];
                    if (same)
                        hits.push_back(k);
                }
                if (hits.size() != 1)
                    break;
                ranges.emplace_back(hits[0], hits[0] + n);
            }
            if (ranges.size() != patterns.size())
                continue;
            std::vector<long long> calls, carriers;
            for (long long k = begin; k < end; k++)
                if (W(k) == "playfxontag")
                    calls.push_back(k);
            for (long long k = begin; k < end - 2; k++)
                if (W(k) == "self" && W(k + 1) == "." && W(k + 2) == "fx")
                    carriers.push_back(k);
            if (calls.size() != 1 || carriers.size() != 6)
                continue;
            const auto call = calls[0];
            if (Argc(tokens, call) != 3)
                continue;
            const auto close = static_cast<long long>(Match(tokens, static_cast<size_t>(call + 1), "(", ")"));
            if (!(W(close - 5) == "self" && W(close - 4) == "." && W(close - 3) == "fx" && W(close - 2) == "," && W(close - 1) == "fxtag")
                || W(close - 6) != ",")
                continue;
            At(tokens, call).text = std::string(COMPAT) + "::waw_playfx";
            At(tokens, call).kind = Kind::Punct;
            At(tokens, close - 5).text = "self.origin, anglestoforward(self.angles), anglestoup(self.angles)";
            At(tokens, close - 5).kind = Kind::Punct;
            Blank(tokens, close - 4, close);
            for (const auto& [a, b] : ranges)
                Blank(tokens, a, b);
            changed++;
        }
        return changed;
    }
} // namespace w2b::gsc::port

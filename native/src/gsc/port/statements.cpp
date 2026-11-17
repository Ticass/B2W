#include "statements.h"

#include "report.h"

namespace w2b::gsc::port
{
    std::vector<Span> TopLevelStatements(const Tokens& tokens, const long long open, const long long close)
    {
        std::vector<Span> spans;
        long long start = open + 1;
        int depth = 0;
        long long skipTo = 0;
        for (long long i = open + 1; i < close; i++)
        {
            if (i < skipTo)
                continue;
            const auto& t = At(tokens, i).text;
            if (t == "/#" && depth == 0 && start == i)
            {
                long long j = i + 1;
                while (j < close && At(tokens, j).text != "#/")
                    j++;
                skipTo = j + 1;
                start = skipTo;
                continue;
            }
            depth += In(t, {"(", "[", "{"});
            depth -= In(t, {")", "]", "}"});
            if (depth || !In(t, {";", "}"}) || At(tokens, i + 1).Low() == "else")
                continue;
            spans.emplace_back(start, i + 1);
            start = i + 1;
        }
        return spans;
    }

    std::pair<std::set<std::string>, std::set<std::string>> StatementLocals(const Tokens& tokens, const long long start, const long long end)
    {
        std::set<std::string> assigned, used;
        for (long long i = start; i < end; i++)
        {
            const auto& t = At(tokens, i);
            if (t.kind != Kind::Ident || (i > start && In(At(tokens, i - 1).text, {".", "::", "\\"})))
                continue;
            const std::string next = i + 1 < end ? At(tokens, i + 1).text : "";
            if (next == "(" || next == "::")
                continue;
            if (In(next, {"=", "++", "--", "+=", "-=", "*=", "/="}))
            {
                assigned.insert(t.Low());
                if (next == "=")
                    continue;
            }
            else if (next == "[")
            {
                int depth = 0;
                long long j = i + 1;
                while (j < end)
                {
                    depth += At(tokens, j).text == "[";
                    depth -= At(tokens, j).text == "]";
                    if (!depth)
                        break;
                    j++;
                }
                if (j + 1 < end && At(tokens, j + 1).text == "=")
                    assigned.insert(t.Low());
            }
            used.insert(t.Low());
        }
        return {assigned, used};
    }

    std::set<size_t> LocalDependencies(const Tokens& tokens, const std::vector<Span>& spans, const std::set<size_t>& keep)
    {
        std::vector<std::pair<std::set<std::string>, std::set<std::string>>> info;
        for (const auto& [s, e] : spans)
            info.push_back(StatementLocals(tokens, s, e));
        std::set<size_t> extra;
        std::vector<size_t> todo(keep.begin(), keep.end());
        while (!todo.empty())
        {
            const auto n = todo.back();
            todo.pop_back();
            const auto& needed = info[n].second;
            for (size_t m = 0; m < n; m++)
            {
                if (keep.count(m) || extra.count(m))
                    continue;
                bool shares = false;
                for (const auto& name : info[m].first)
                    if (needed.count(name))
                    {
                        shares = true;
                        break;
                    }
                if (shares)
                {
                    extra.insert(m);
                    todo.push_back(m);
                }
            }
        }
        return extra;
    }

    bool ScriptBranchTaken(const Tokens& tokens, const long long bodyOpen, const long long bodyClose, const long long index,
                           const std::optional<std::string>& mapName)
    {
        if (!mapName || mapName->empty())
            return true;
        for (const auto& [a, b] : TopLevelStatements(tokens, bodyOpen, bodyClose))
        {
            if (!(a <= index && index < b) || At(tokens, a).Low() != "if")
                continue;
            const auto guardEnd = static_cast<long long>(Match(tokens, static_cast<size_t>(a + 1), "(", ")"));
            std::vector<std::string> words;
            for (long long k = a + 2; k < guardEnd; k++)
                words.push_back(At(tokens, k).Low());
            if (words.size() != 5 || words[0] != "level" || words[1] != "." || words[2] != "script" || !In(words[3], {"==", "!="})
                || !StartsWith(words[4], "\"") || At(tokens, guardEnd + 1).text != "{")
                return true;
            const bool matched = (Lower(*mapName) == StripChars(words[4], "\"")) == (words[3] == "==");
            const auto yesEnd = static_cast<long long>(Match(tokens, static_cast<size_t>(guardEnd + 1), "{", "}"));
            return index <= yesEnd ? matched : !matched;
        }
        return true;
    }

    py::Json PortReport::ToJson() const
    {
        const auto dictOfLists = [](const OrderedMap<std::vector<std::string>>& m)
        {
            py::Json out = py::Json::object();
            for (const auto& k : m.order)
                out[k] = m.values.at(k);
            return out;
        };
        py::Json j = py::Json::object();
        j["map_main"] = mapMain;
        j["ported"] = ported;
        j["core_overrides"] = coreOverrides;
        j["characters"] = characters;
        j["weapon_registration"] = weaponRegistration;
        j["extracted"] = extracted;
        j["unsupported"] = dictOfLists(unsupported);
        py::Json counts = py::Json::object();
        for (const auto& k : rewrites.order)
            counts[k] = rewrites.values.at(k);
        j["rewrites"] = counts;
        j["fx"] = fx;
        j["animtrees"] = dictOfLists(animtrees);
        j["errors"] = errors;
        j["waw_hud_materials"] = wawHudMaterials;
        py::Json trees = py::Json::object();
        for (const auto& k : nativeActorTrees.order)
            trees[k] = nativeActorTrees.values.at(k);
        j["native_actor_trees"] = trees;
        j["client_listener_strings"] = clientListenerStrings;
        return j;
    }
} // namespace w2b::gsc::port

#include "gsc/port/zone_scripts.h"

#include "core/pyre.h"
#include "gsc/port/util.h"

#include <cmath>
#include <map>
#include <span>

namespace w2b::gsc::port
{
    namespace
    {
        using Argument = std::span<const Token>;

        bool KeepRetailToken(const Token& token, bool& developer)
        {
            if (token.text == "/#" || token.text == "#/")
            {
                developer = token.text == "/#";
                return false;
            }
            return !developer && token.kind != Kind::Eof;
        }

        std::vector<Argument> Arguments(Argument tokens)
        {
            std::vector<Argument> args;
            int depth = 0;
            std::size_t start = 0, i = 0;
            for (; i < tokens.size(); ++i)
            {
                const auto& text = tokens[i].text;
                if (In(text, {"(", "[", "[["}))
                    ++depth;
                else if (In(text, {")", "]", "]]"}))
                {
                    if (depth == 0)
                        break;
                    --depth;
                }
                if (text == "," && depth == 0)
                {
                    args.push_back(tokens.subspan(start, i - start));
                    start = i + 1;
                }
            }
            if (i > start)
                args.push_back(tokens.subspan(start, i - start));
            return args;
        }

        class LocalFlags
        {
        public:
            void ReadAssignment(const Token& variable, Argument next)
            {
                if (next.size() >= 6 && next[0].text == "[" && next[1].kind == Kind::Number &&
                    next[2].text == "]" && next[3].text == "=" && next[4].kind == Kind::String && next[5].text == ";")
                {
                    double index = 0;
                    if (!py::ParseFloat(next[1].text, index) || !std::isfinite(index))
                        throw std::runtime_error("invalid zone flag array index: " + Repr(next[1].text));
                    arrays_[variable.Low()][std::trunc(index)] = next[4].text;
                }
                else if (next.size() >= 3 && next[0].text == "=" && next[1].kind == Kind::String && next[2].text == ";")
                {
                    auto key = variable.Low();
                    arrays_.erase(key);
                    scalars_[std::move(key)] = next[1].text;
                }
                else if (next.size() >= 4 && next[0].text == "=" && next[1].text == "[" &&
                         next[2].text == "]" && next[3].text == ";")
                    arrays_[variable.Low()].clear();
            }

            std::string Render(Argument argument) const
            {
                if (argument.size() == 1 && argument.front().kind == Kind::Ident)
                {
                    const auto key = argument.front().Low();
                    if (const auto it = arrays_.find(key); it != arrays_.end())
                    {
                        std::vector<std::string> values;
                        for (const auto& element : it->second)
                            values.push_back(element.second);
                        return "[ " + Join(values, ", ") + " ]";
                    }
                    if (const auto it = scalars_.find(key); it != scalars_.end())
                        return it->second;
                }
                std::vector<std::string> words;
                for (const auto& token : argument)
                    words.push_back(token.text);
                return Replace(Replace(Join(words, " "), " ( ", "( "), " ,", ",");
            }

        private:
            std::map<std::string, std::map<double, std::string>> arrays_;
            std::map<std::string, std::string> scalars_;
        };

        const auto& QuotedStrings()
        {
            static const pyre::Regex pattern(R"re("([^"]+)")re");
            return pattern;
        }

        void ReadInitial(ZoneGraph& graph, std::string_view live)
        {
            static const pyre::Regex pattern(
                R"re(^\s*(?:zones|init_zones|level\s*\.\s*dlc3\s*\.\s*initialzones)\s*\[[^\]]*\]\s*=\s*"([^"]+)"\s*;)re",
                pyre::MULTILINE | pyre::IGNORECASE);
            std::set<std::string> seen;
            for (const auto& match : pattern.FindAll(live))
                if (seen.insert(match.front()).second)
                    graph.initial.push_back(match.front());
            if (graph.initial.empty())
            {
                const auto defaultZone = QuotedStrings().Search(graph.adjacency.front());
                if (!defaultZone)
                    throw std::runtime_error("first zone link is missing a quoted zone name");
                graph.initial.push_back((*defaultZone)[1]);
            }
        }
    }

    ZoneGraph ReadZoneGraph(std::string_view source)
    {
        auto sourceTokens = Tokenize(source);
        Tokens tokens;
        tokens.reserve(sourceTokens.size());
        bool developer = false;
        for (auto& token : sourceTokens)
            if (KeepRetailToken(token, developer))
                tokens.push_back(std::move(token));
        ZoneGraph graph;
        LocalFlags locals;
        std::string live;
        live.reserve(source.size());
        int depth = 0;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            const auto& token = tokens[i];
            if (i > 0)
                live += token.line == tokens[i - 1].line ? ' ' : '\n';
            live += token.text;

            if (token.text == "{")
                ++depth;
            else if (token.text == "}" && --depth == 0)
                locals = {};

            const auto previous = i == 0 ? std::string_view{} : std::string_view(tokens[i - 1].text);
            const auto following = Argument(tokens).subspan(i + 1);
            if (token.kind == Kind::Ident && (i == 0 || In(previous, {";", "{", "}"})))
                locals.ReadAssignment(token, following);

            if (token.kind != Kind::Ident || previous == "." || following.empty() ||
                following.front().text != "(" || token.Low() != "add_adjacent_zone")
                continue;
            std::vector<std::string> args;
            for (const auto argument : Arguments(following.subspan(1)))
                args.push_back(locals.Render(argument));
            graph.adjacency.push_back(Join(args, ", "));
        }
        if (!graph.adjacency.empty())
            ReadInitial(graph, live);
        return graph;
    }

    std::set<std::string> ZoneGraph::Names() const
    {
        std::set<std::string> names(initial.begin(), initial.end());
        for (const auto& row : adjacency)
        {
            const auto matches = QuotedStrings().FindAll(row);
            for (std::size_t i = 0; i < std::min<std::size_t>(2, matches.size()); ++i)
                names.insert(matches[i].front());
        }
        return names;
    }

    py::Json ZoneGraph::ToJson() const
    {
        return {{"initial", initial}, {"adjacency", adjacency}, {"names", Names()}};
    }
}

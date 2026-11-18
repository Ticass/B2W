#include "models/identities.h"

#include "gsc/parse.h"

#include <set>

namespace w2b::models
{
    namespace
    {
        constexpr std::pair<std::string_view, int> tagArguments[] = {
            {"gettagorigin", 0}, {"gettagangles", 0}, {"hidepart", 0}, {"showpart", 0},
            {"attach", 1}, {"detach", 1}, {"linkto", 1}, {"linktoblendtotag", 1},
            {"playfxontag", 2}, {"waw_gettagorigin", 0}, {"waw_gettagangles", 0},
            {"waw_attach", 1}, {"waw_detach", 1}, {"waw_linkto", 1}, {"waw_playfxontag", 2},
            {"playviewmodelfx", 2}
        };

        void MarkTagArguments(const std::vector<gsc::Token>& tokens, std::vector<bool>& tags)
        {
            for (const auto& ref : gsc::References(tokens))
            {
                if (ref.pointer)
                    continue;
                const auto name = gsc::Lower(ref.name);
                const auto call = std::ranges::find(tagArguments, name, &std::pair<std::string_view, int>::first);
                if (call == std::end(tagArguments))
                    continue;
                int argument = 0, depth = 0;
                for (std::size_t i = ref.index + 2; i < tokens.size(); ++i)
                {
                    const auto& value = tokens[i].text;
                    if ((value == ")" || value == ",") && depth == 0)
                    {
                        if (value == ")" || ++argument > call->second)
                            break;
                    }
                    else if (value == "(" || value == "[" || value == "{")
                        ++depth;
                    else if (value == ")" || value == "]" || value == "}")
                        --depth;
                    if (argument == call->second)
                        tags[i] = true;
                }
            }
        }

        void MarkTagAssignments(const std::vector<gsc::Token>& tokens, std::vector<bool>& tags)
        {
            std::set<std::string, std::less<>> variables;
            for (std::size_t i = 1; i + 1 < tokens.size(); ++i)
                if (tags[i] && tokens[i].kind == gsc::Kind::Ident &&
                    (tokens[i - 1].text == "(" || tokens[i - 1].text == ",") &&
                    (tokens[i + 1].text == ")" || tokens[i + 1].text == ","))
                    variables.insert(tokens[i].Low());
            if (variables.empty())
                return;
            for (std::size_t i = 2; i < tokens.size(); ++i)
                if (tokens[i].kind == gsc::Kind::String && tokens[i - 1].text == "=" &&
                    tokens[i - 2].kind == gsc::Kind::Ident && variables.contains(tokens[i - 2].Low()) &&
                    (i < 3 || (tokens[i - 3].text != "." && tokens[i - 3].text != "]")))
                    tags[i] = true;
        }
    }

    std::string RewriteModelLiterals(std::string_view source, const ModelNames& renamed)
    {
        auto tokens = gsc::Tokenize(source);
        const auto model = [&](const auto& token)
        {
            return renamed.find(std::string_view(token.text).substr(1, token.text.size() - 2));
        };
        if (std::ranges::none_of(tokens, [&](const auto& token)
        {
            return token.kind == gsc::Kind::String && model(token) != renamed.end();
        }))
            return gsc::Emit(tokens);
        std::vector<bool> tags(tokens.size(), false);
        MarkTagArguments(tokens, tags);
        MarkTagAssignments(tokens, tags);
        for (std::size_t i = 0; i < tokens.size(); ++i)
            if (!tags[i] && tokens[i].kind == gsc::Kind::String)
                if (const auto rename = model(tokens[i]); rename != renamed.end())
                    tokens[i].text = '"' + rename->second + '"';
        return gsc::Emit(tokens);
    }
}

#include "materials/techsets.h"

#include <algorithm>
#include <array>

namespace w2b::materials
{
    namespace
    {
        constexpr bool Digit(char value) { return value >= '0' && value <= '9'; }
        constexpr bool Letter(char value) { return value >= 'a' && value <= 'z'; }

        bool LayerToken(std::string_view token)
        {
            if (token.size() < 2 || token.size() % 2 || std::string_view("rbtam").find(token[0]) == token.npos ||
                !Digit(token[1]))
                return false;
            for (std::size_t i = 2; i < token.size(); i += 2)
                if (!Letter(token[i]) || !Digit(token[i + 1]))
                    return false;
            return true;
        }

        std::vector<std::string_view> Split(std::string_view token)
        {
            std::vector<std::string_view> parts;
            for (auto next = token.find('_'); next != token.npos; next = token.find('_'))
            {
                parts.push_back(token.substr(0, next));
                token.remove_prefix(next + 1);
            }
            parts.push_back(token);
            return parts;
        }
    }

    Technique ParseTechnique(std::string_view name)
    {
        Technique result{std::string(name), Family::other, false, {}, {}, {}};
        constexpr std::pair<std::string_view, Family> prefixes[]{{"wpc_", Family::world}, {"wc_", Family::world},
            {"mlv_", Family::modelVertex}, {"mc_", Family::model}};
        for (const auto& [prefix, family] : prefixes)
            if (name.starts_with(prefix))
            {
                result.family = family;
                name.remove_prefix(prefix.size());
                break;
            }
        auto tokens = Split(name);
        const auto last = tokens.back();
        if (last.size() == 8 && std::ranges::all_of(last, [](char c) { return Letter(c) || Digit(c); }) &&
            !LayerToken(last))
            tokens.pop_back();
        result.lit = tokens.size() >= 2 && (tokens[0] == "l" || tokens[0] == "lit") &&
                     (tokens[1] == "sm" || tokens[1] == "hsm");
        if (!result.lit)
        {
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (i)
                    result.unlit += '_';
                result.unlit += tokens[i];
            }
            return result;
        }
        if (result.family == Family::other)
            result.family = Family::layered;
        for (std::size_t i = 2; i < tokens.size(); ++i)
        {
            const auto token = tokens[i];
            if (!LayerToken(token))
            {
                result.modifiers.emplace_back(token);
                continue;
            }
            Layer layer{token[0], static_cast<unsigned>(token[1] - '0'), {}};
            for (std::size_t ch = 2; ch < token.size(); ch += 2)
                layer.features.insert(token[ch]);
            result.layers.push_back(std::move(layer));
        }
        return result;
    }

    std::vector<std::string> LastResort(std::string_view name)
    {
        const auto parsed = ParseTechnique(name);
        if (parsed.lit || (parsed.family != Family::world && parsed.family != Family::model &&
                           parsed.family != Family::modelVertex))
            return {};
        const std::string prefix = parsed.family == Family::world ? "wc_" : "mc_";
        const auto& kind = parsed.unlit;
        char first = kind.find("water") != kind.npos ? 'b' : 't';
        for (const auto part : Split(kind))
            if (part == "r0c0" || part == "t0c0" || part == "b0c0")
            {
                first = part[0];
                break;
            }
        std::vector<std::string> result;
        const bool unlitFirst = kind.find("unlit") != kind.npos || kind.find("add") != kind.npos;
        const auto addUnlit = [&] { result.insert(result.end(), {prefix + "unlit_blend", prefix + "unlit"}); };
        if (unlitFirst)
            addUnlit();
        std::set<char> seen;
        for (const auto blend : {first, 't', 'b', 'r'})
            if (seen.insert(blend).second)
                result.push_back(prefix + "l_sm_" + blend + "0c0");
        if (!unlitFirst)
            addUnlit();
        return result;
    }
}

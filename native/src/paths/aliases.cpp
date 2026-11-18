#include "paths/aliases.h"

#include "core/pyre.h"
#include "core/unicode.h"
#include "gsc/tokens.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

namespace w2b::paths
{
    namespace
    {
        constexpr double MantleHeight = 48;
        constexpr std::array Down{40, 72, 96, 127, 176};
        constexpr std::array Up{72, 96, 127, 154, 222};
        constexpr std::string_view Climb = "jump_up_to_climb";

        bool Contains(std::string_view name, std::string_view part)
        {
            return name.find(part) != name.npos;
        }

        bool IsMantle(std::string_view name)
        {
            return Contains(name, "mantle") || Contains(name, "hop") || Contains(name, "window") || Contains(name, "barrier");
        }

        std::optional<double> Number(std::string_view name)
        {
            static const pyre::Regex number(R"(\d+)");
            const auto match = number.Search(name);
            if (!match)
                return std::nullopt;
            double value{};
            if (!py::ParseFloat((*match)[0], value))
                throw Error("invalid traversal height in " + std::string(name));
            return value;
        }

        template<typename Values, typename Height>
        auto Nearest(const Values& candidates, double value, Height height)
        {
            return std::ranges::min_element(candidates, {}, [&](const auto& candidate) {
                return std::abs(std::invoke(height, candidate) - value);
            });
        }

        std::string DownFor(double value)
        {
            return "jump_down_" + std::to_string(*Nearest(Down, value, std::identity{}));
        }

        std::string UpFor(double value)
        {
            if (value > Up.back() + 24)
                return std::string(Climb);
            return "jump_up_" + std::to_string(*Nearest(Up, value, std::identity{}));
        }

        std::string FirstWord(std::string_view line)
        {
            const auto decoded = unicode::Decode(line);
            const auto begin = std::ranges::find_if_not(decoded, unicode::IsSpace);
            const auto end = std::find_if(begin, decoded.end(), unicode::IsSpace);
            return unicode::Encode(std::u32string_view(begin, end));
        }
    }

    Names TraverseSubstates(std::string_view text)
    {
        Names walking, crawling;
        // A state starts at column zero, with its opening and closing braces
        // at the start of their own lines. Skip whole unrelated states.
        for (std::size_t line = 0; line < text.size();)
        {
            const auto newline = text.find('\n', line);
            const auto start = line;
            line = newline == text.npos ? text.size() : newline + 1;
            const auto head = text.substr(start, line - start);
            const auto first = FirstWord(head);
            if (first.empty() || head.empty() || py::IsSpace(head.front()))
                continue;
            auto name = std::string_view(first);
            if (const auto colon = name.find(':'); colon != name.npos)
                name = name.substr(0, colon);
            const auto decoded = unicode::Decode(name);
            if (decoded.empty() || !std::ranges::all_of(decoded, unicode::IsWord))
                continue;
            const auto rest = text.substr(start + name.size());
            const auto colon = rest.find_first_not_of(" \t\r\n\v\f\x1c\x1d\x1e\x1f");
            if (colon == rest.npos || rest[colon] != ':')
                continue;
            const auto endHead = text.find('\n', start + name.size() + colon);
            if (endHead == text.npos || endHead + 1 == text.size() || text[endHead + 1] != '{')
                continue;
            const auto close = text.find("\n}", endHead + 2);
            if (close == text.npos)
                continue;
            const auto endLine = text.find('\n', close + 2);
            line = endLine == text.npos ? text.size() : endLine + 1;
            if (name != "zm_traverse" && name != "zm_traverse_crawl")
                continue;
            auto& list = name == "zm_traverse" ? walking : crawling;
            list.clear();
            for (const auto entry : py::SplitLines(text.substr(endHead + 2, close - endHead - 2)))
                if (auto word = FirstWord(entry); !word.empty() && !word.starts_with("//"))
                    list.insert(std::move(word));
        }
        Names result;
        for (const auto& alias : walking)
            if (crawling.contains(alias + "_crawl"))
                result.insert(alias);
        return result;
    }

    Aliases::Aliases(const std::optional<Names>& substates) : substates_(substates)
    {
        if (!substates_)
            return;
        for (const auto& alias : *substates_)
        {
            const auto fill = [&](std::string_view prefix, Heights& heights) {
                const auto name = std::string_view(alias);
                if (!name.starts_with(prefix))
                    return;
                const auto digits = unicode::Decode(name.substr(prefix.size()));
                long long number{};
                bool overflow{};
                if (!digits.empty() && std::ranges::all_of(digits, unicode::IsDigit) &&
                    py::ParseInt(name.substr(prefix.size()), number, overflow) && !overflow &&
                    number <= std::numeric_limits<int>::max())
                    heights[static_cast<int>(number)] = alias;
            };
            fill("jump_down_", down_);
            fill("jump_up_", up_);
        }
    }

    Alias Aliases::Measured(std::string_view name, std::optional<double> height, std::optional<double> number) const
    {
        if (IsMantle(name))
            return std::nullopt;
        const auto same = name.starts_with("zombie_") ? name.substr(7) : name;
        if (substates_->contains(same))
            return std::string(same);
        if (!height && number)
            height = *number * (Contains(name, "down") ? -1 : 1);
        if (!height || (std::abs(*height) <= MantleHeight && !Contains(name, "down") && !Contains(name, "up") && !Contains(name, "climb")))
            return std::nullopt;
        if (*height < 0 && !down_.empty())
            return Nearest(down_, -*height, &Heights::value_type::first)->second;
        if (*height >= 0 && !up_.empty())
        {
            const auto best = Nearest(up_, *height, &Heights::value_type::first);
            if (std::abs(best->first - *height) > 24 && substates_->contains("jump_up_2_climb"))
                return "jump_up_2_climb";
            return best->second;
        }
        return std::nullopt;
    }

    Alias Aliases::Select(std::string_view source, std::optional<double> height) const
    {
        const auto name = gsc::Lower(source);
        const auto number = Number(name);
        if (substates_)
            return Measured(name, height, number);
        if (Contains(name, "climb"))
            return std::string(Climb);
        if (Contains(name, "down"))
            return DownFor(number.value_or(std::abs(height.value_or(0))));
        if (Contains(name, "up"))
            return UpFor(number.value_or(std::abs(height.value_or(0))));
        if (IsMantle(name) || !height || std::abs(*height) <= MantleHeight)
            return std::nullopt;
        return *height < 0 ? DownFor(-*height) : UpFor(*height);
    }
}

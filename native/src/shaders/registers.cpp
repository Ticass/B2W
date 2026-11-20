#include "registers.h"

#include "core/text.h"

namespace w2b::shaders
{
    namespace
    {
        bool IsSwizzleChar(const char c)
        {
            return std::string_view("xyzwrgba").find(c) != std::string_view::npos;
        }

        // end of prefix + one or more digits at text[at], or npos
        size_t Digits(const std::string_view text, const size_t at)
        {
            size_t end = at;
            while (end < text.size() && util::IsDigit(text[end]))
                end++;
            return end > at ? end : std::string_view::npos;
        }

        // Ends of the register alternatives at text[at], in regex order.
        // (\d+ is greedy; a shorter digit run is always followed by a digit,
        // which no continuation in these patterns accepts.)
        std::vector<size_t> Alternatives(const std::string_view text, const size_t at)
        {
            std::vector<size_t> ends;
            if (at >= text.size())
                return ends;
            const char c = text[at];
            if (c == 'r' || c == 'v' || c == 'c')
            {
                if (const auto end = Digits(text, at + 1); end != std::string_view::npos)
                    ends.push_back(end);
            }
            if (c == 'o')
            {
                if (at + 1 < text.size() && text[at + 1] == 'C')
                    if (const auto end = Digits(text, at + 2); end != std::string_view::npos)
                        ends.push_back(end);
                if (const auto end = Digits(text, at + 1); end != std::string_view::npos)
                    ends.push_back(end);
                if (text.substr(at, 6) == "oDepth")
                    ends.push_back(at + 6);
            }
            return ends;
        }
    } // namespace

    bool MatchRegister(const std::string_view text, Register& out)
    {
        for (const auto end : Alternatives(text, 0))
        {
            const auto rest = text.substr(end);
            if (rest.empty())
            {
                out = {std::string(text.substr(0, end)), "", false};
                return true;
            }
            if (rest[0] == '.' && rest.size() >= 2 && rest.size() <= 5)
            {
                bool ok = true;
                for (const char c : rest.substr(1))
                    ok = ok && IsSwizzleChar(c);
                if (ok)
                {
                    out = {std::string(text.substr(0, end)), std::string(rest.substr(1)), true};
                    return true;
                }
            }
        }
        return false;
    }

    bool MatchAbsModifier(const std::string_view text, std::string& base, std::string& swizzle)
    {
        if (text.empty() || (text[0] != 'r' && text[0] != 'v' && text[0] != 'c'))
            return false;
        const auto end = Digits(text, 1);
        if (end == std::string_view::npos || text.substr(end, 4) != "_abs")
            return false;
        const auto rest = text.substr(end + 4);
        if (!rest.empty())
        {
            if (rest[0] != '.' || rest.size() < 2 || rest.size() > 5)
                return false;
            for (const char c : rest.substr(1))
                if (!IsSwizzleChar(c))
                    return false;
        }
        base = std::string(text.substr(0, end));
        swizzle = std::string(rest);
        return true;
    }

    bool MatchSemantic(const std::string_view text)
    {
        if (text.empty() || !(text[0] >= 'A' && text[0] <= 'Z'))
            return false;
        for (const char c : text.substr(1))
            if (!((c >= 'A' && c <= 'Z') || util::IsDigit(c) || c == '_'))
                return false;
        return true;
    }

    std::vector<std::string> FindRegisters(const std::string_view text)
    {
        std::vector<std::string> found;
        size_t at = 0;
        while (at < text.size())
        {
            // \b: the alternatives start with a word character
            if (at > 0 && util::IsWord(text[at - 1]))
            {
                at++;
                continue;
            }
            bool matched = false;
            for (const auto end : Alternatives(text, at))
            {
                const bool lookahead = end == text.size() || text[end] == '.' || text[end] == '_' || text[end] == ',' || text[end] == ')'
                                       || util::IsSpace(text[end]);
                if (lookahead)
                {
                    found.emplace_back(text.substr(at, end - at));
                    at = end;
                    matched = true;
                    break;
                }
            }
            if (!matched)
                at++;
        }
        return found;
    }

    std::string SwizzleXyzw(const std::string_view swizzle)
    {
        std::string out(swizzle);
        for (auto& c : out)
        {
            if (c == 'r')
                c = 'x';
            else if (c == 'g')
                c = 'y';
            else if (c == 'b')
                c = 'z';
            else if (c == 'a')
                c = 'w';
        }
        return out;
    }
} // namespace w2b::shaders

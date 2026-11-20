#pragma once
// Python list/str semantics the translator relies on, over token vectors.

#include "gsc/parse.h"
#include "core/text.h"

#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::gsc::port
{
    using Tokens = std::vector<Token>;
    using util::Json;

    // tokens[i] with Python's negative indexing; IndexError past either end
    // IndexError: list index out of range (W2B_STACK=1 prints the call stack)
    [[noreturn]] void ThrowIndexError(long long index, size_t size);

    inline Token& At(Tokens& tokens, long long i)
    {
        if (i < 0)
            i += static_cast<long long>(tokens.size());
        if (i < 0 || i >= static_cast<long long>(tokens.size()))
            ThrowIndexError(i, tokens.size());
        return tokens[static_cast<size_t>(i)];
    }
    inline const Token& At(const Tokens& tokens, long long i)
    {
        if (i < 0)
            i += static_cast<long long>(tokens.size());
        if (i < 0 || i >= static_cast<long long>(tokens.size()))
            ThrowIndexError(i, tokens.size());
        return tokens[static_cast<size_t>(i)];
    }

    // Python slice bounds [a:b] clamped to [0, n]
    inline std::pair<size_t, size_t> Slice(const size_t n, long long a, long long b)
    {
        const auto clamp = [n](long long v)
        {
            if (v < 0)
                v += static_cast<long long>(n);
            return static_cast<size_t>(std::max(0LL, std::min<long long>(v, static_cast<long long>(n))));
        };
        const auto lo = clamp(a), hi = clamp(b);
        return {lo, std::max(lo, hi)};
    }

    inline bool In(const std::string_view value, const std::initializer_list<std::string_view> options)
    {
        for (const auto option : options)
            if (value == option)
                return true;
        return false;
    }

    inline bool Contains(const std::string_view haystack, const std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    inline bool StartsWith(const std::string_view text, const std::string_view prefix)
    {
        return text.substr(0, prefix.size()) == prefix;
    }

    inline bool EndsWith(const std::string_view text, const std::string_view suffix)
    {
        return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
    }

    // gsc.emit(tokens[a:b])
    std::string EmitRange(const Tokens& tokens, long long a, long long b);

    // "".join(t.low for t in tokens[a:b]) / t.text
    std::string JoinLow(const Tokens& tokens, long long a, long long b);
    std::string JoinText(const Tokens& tokens, long long a, long long b);

    // str.strip / lstrip / rstrip (Unicode whitespace) and str.strip(chars)
    std::string PyStrip(std::string_view text);
    std::string PyLStrip(std::string_view text);
    std::string PyRStrip(std::string_view text);
    std::string StripChars(std::string_view text, std::string_view chars);
    std::string RStripChars(std::string_view text, std::string_view chars);

    // [t.low for t in tokens[a:b]] == words
    bool WordsEqual(const Tokens& tokens, long long a, const std::vector<std::string>& words);

    // The repeated "receiver path back over a.b[c].d" scan of the bridges
    long long PathStart(const Tokens& tokens, long long dot);

    std::string Replace(std::string text, std::string_view from, std::string_view to);
    std::string Join(const std::vector<std::string>& parts, std::string_view sep);
} // namespace w2b::gsc::port

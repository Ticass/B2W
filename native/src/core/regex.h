#pragma once
// A backtracking regular expression engine with Python `re` semantics, for
// the patterns the converter's scripts and tables use: ordered alternation, greedy and
// lazy quantifiers, groups (named, non-capturing), lookahead/lookbehind,
// backreferences, \A \Z ^ $ \b \B, \d \w \s (Unicode), classes, and the
// i/m/s/x flags (global or scoped). Text is UTF-8; positions are code points.

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace w2b::regex
{
    enum Flags : unsigned
    {
        IGNORECASE = 2,
        MULTILINE = 8,
        DOTALL = 16,
        VERBOSE = 64,
    };

    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Program;

    class Match
    {
    public:
        // group(n) as UTF-8; nullopt when the group did not take part
        std::optional<std::string> Group(size_t n = 0) const;
        std::optional<std::string> Group(const std::string& name) const;
        std::string operator[](size_t n) const; // group(n) or ""
        long long Start(size_t n = 0) const;    // code point offsets, -1 when unset
        long long End(size_t n = 0) const;
        size_t Groups() const; // number of capture groups

        std::shared_ptr<const Program> program;
        std::shared_ptr<const std::u32string> text;
        std::vector<long long> spans; // 2 per group (group 0 first)
    };

    class Regex
    {
    public:
        Regex() = default;
        explicit Regex(std::string_view pattern, unsigned flags = 0);

        std::optional<Match> Search(std::string_view text, size_t pos = 0) const;
        std::optional<Match> MatchAt(std::string_view text, size_t pos = 0) const; // re.match
        std::optional<Match> FullMatch(std::string_view text) const;
        std::vector<Match> FindIter(std::string_view text) const;
        // re.findall: whole matches, group 1, or all groups joined by '\x1f'
        std::vector<std::vector<std::string>> FindAll(std::string_view text) const;
        // re.sub/re.subn with a template (\1, \g<name>, \n...) or a callback; count 0 = all
        std::string Sub(std::string_view replacement, std::string_view text, size_t count = 0, size_t* made = nullptr) const;
        std::string Sub(const std::function<std::string(const Match&)>& replace, std::string_view text, size_t count = 0, size_t* made = nullptr) const;
        std::vector<std::string> Split(std::string_view text, size_t maxsplit = 0) const;

        size_t Groups() const;

    private:
        std::shared_ptr<const Program> m_program;
        std::optional<Match> Run(const std::shared_ptr<const std::u32string>& text, size_t pos, int mode, bool mustAdvanceFrom, size_t from) const;
    };

    // re.escape
    std::string Escape(std::string_view text);
} // namespace w2b::regex

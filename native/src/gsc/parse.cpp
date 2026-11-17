// Port of gsc.parse / gsc._match / gsc.references / gsc.repair_block_comments.
#include "parse.h"

#include "core/unicode.h"

#include <set>

namespace w2b::gsc
{
    namespace
    {
        bool IEquals(const std::u32string& s, const size_t pos, const std::u32string_view word)
        {
            if (pos + word.size() > s.size())
                return false;
            for (size_t i = 0; i < word.size(); i++)
            {
                char32_t c = s[pos + i];
                if (c >= 'A' && c <= 'Z')
                    c = c - 'A' + 'a';
                if (c != word[i])
                    return false;
            }
            return true;
        }

        // INCLUDE_RE.match: #include\s+([\w\\/]+)\s*;?  (IGNORECASE)
        std::optional<std::string> IncludeOf(const std::string& text)
        {
            const auto s = unicode::Decode(text);
            if (!IEquals(s, 0, U"#include"))
                return std::nullopt;
            size_t pos = 8;
            const size_t spaces = pos;
            while (pos < s.size() && unicode::IsSpace(s[pos]))
                pos++;
            if (pos == spaces)
                return std::nullopt;
            const size_t start = pos;
            while (pos < s.size() && (unicode::IsWord(s[pos]) || s[pos] == '\\' || s[pos] == '/'))
                pos++;
            if (pos == start)
                return std::nullopt;
            auto include = unicode::Encode(s.substr(start, pos - start));
            for (auto& c : include)
                if (c == '/')
                    c = '\\';
            return include;
        }

        // ANIMTREE_RE.match: #using_animtree\s*\(\s*"([^"]+)"\s*\)  (IGNORECASE)
        std::optional<std::string> AnimtreeOf(const std::string& text)
        {
            const auto s = unicode::Decode(text);
            if (!IEquals(s, 0, U"#using_animtree"))
                return std::nullopt;
            size_t pos = 15;
            const auto skip = [&]
            {
                while (pos < s.size() && unicode::IsSpace(s[pos]))
                    pos++;
            };
            skip();
            if (pos >= s.size() || s[pos] != '(')
                return std::nullopt;
            pos++;
            skip();
            if (pos >= s.size() || s[pos] != '"')
                return std::nullopt;
            const size_t start = ++pos;
            while (pos < s.size() && s[pos] != '"')
                pos++;
            if (pos >= s.size() || pos == start)
                return std::nullopt;
            const auto name = unicode::Encode(s.substr(start, pos - start));
            pos++;
            skip();
            if (pos >= s.size() || s[pos] != ')')
                return std::nullopt;
            return name;
        }

        const std::set<std::string> CALL_LIKE{"if", "while", "for", "foreach", "switch", "return", "wait", "waittill", "waittillmatch",
                                              "notify", "endon", "thread", "case", "else", "do"};
    } // namespace

    const Function* Script::Find(const std::string& lowName) const
    {
        for (const auto& [name, function] : functions)
            if (name == lowName)
                return &function;
        return nullptr;
    }

    size_t Match(const std::vector<Token>& tokens, const size_t i, const std::string_view open, const std::string_view close)
    {
        int depth = 0;
        for (size_t j = i; j < tokens.size(); j++)
        {
            if (tokens[j].kind != Kind::Punct)
                continue;
            if (tokens[j].text == open)
                depth++;
            else if (tokens[j].text == close)
            {
                depth--;
                if (depth == 0)
                    return j;
            }
        }
        throw GscSyntaxError("line " + std::to_string(tokens[i].line) + ": unbalanced " + std::string(open));
    }

    Script Parse(const std::string_view source, const std::string& path)
    {
        Script script;
        script.path = path;
        try
        {
            script.tokens = Tokenize(source);
        }
        catch (const GscSyntaxError& error)
        {
            throw GscSyntaxError(path + ": " + error.what());
        }
        const auto& tokens = script.tokens;
        size_t i = 0;
        int depth = 0;
        while (i < tokens.size())
        {
            const auto& t = tokens[i];
            if (t.kind == Kind::Directive)
            {
                if (auto include = IncludeOf(t.text))
                    script.includes.push_back(std::move(*include));
                if (auto tree = AnimtreeOf(t.text))
                    script.animtree = std::move(*tree);
            }
            else if (t.kind == Kind::Punct && t.text == "/#" && depth == 0)
            {
            }
            else if (depth == 0 && t.kind == Kind::Ident && i + 1 < tokens.size() && tokens[i + 1].text == "(")
            {
                const auto close = Match(tokens, i + 1, "(", ")");
                if (close + 1 < tokens.size() && tokens[close + 1].text == "{")
                {
                    const auto bodyClose = Match(tokens, close + 1, "{", "}");
                    Function function{t.text, {}, i, close + 1, bodyClose, script.animtree};
                    for (size_t k = i + 2; k < close; k++)
                        if (tokens[k].kind == Kind::Ident)
                            function.params.push_back(tokens[k].text);
                    const auto low = t.Low();
                    if (!script.Find(low))
                        script.functions.emplace_back(low, std::move(function));
                    i = bodyClose + 1;
                    continue;
                }
            }
            if (t.kind == Kind::Punct && t.text == "{")
                depth++;
            else if (t.kind == Kind::Punct && t.text == "}")
                depth--;
            i++;
        }
        return script;
    }

    std::vector<Ref> References(const std::vector<Token>& tokens, const size_t start, const std::optional<size_t> endOpt)
    {
        const size_t end = endOpt.value_or(tokens.size());
        std::vector<Ref> refs;
        for (size_t i = start; i < end; i++)
        {
            const auto& t = tokens[i];
            if (t.kind != Kind::Ident)
                continue;
            long long p = static_cast<long long>(i) - 1;
            while (p >= 0 && tokens[static_cast<size_t>(p)].text.empty() && tokens[static_cast<size_t>(p)].kind == Kind::Punct)
                p--;
            const Token* prev = p >= 0 ? &tokens[static_cast<size_t>(p)] : nullptr;
            const Token& next = tokens[i + 1];
            if (prev && prev->text == "::")
            {
                if (i >= 2 && tokens[i - 2].kind == Kind::Path)
                    refs.push_back({i, t.text, tokens[i - 2].text, i - 2, false, next.text != "("});
                else
                    refs.push_back({i, t.text, std::nullopt, std::nullopt, false, next.text != "("});
                continue;
            }
            if (next.text != "(" || CALL_LIKE.count(t.Low()))
                continue;
            if (prev && prev->text == ".")
                continue;
            static const std::set<std::string> notReceivers{"thread", "return", "else", "wait", "case", "do", "in"};
            bool method = prev
                          && (((prev->kind == Kind::Ident || prev->kind == Kind::String) && !notReceivers.count(prev->Low())) || prev->text == ")"
                              || prev->text == "]");
            if (method && prev->text == ")")
            {
                int depth = 0;
                for (long long k = p; k >= 0; k--)
                {
                    const auto& tk = tokens[static_cast<size_t>(k)];
                    if (tk.text == ")")
                        depth++;
                    else if (tk.text == "(")
                    {
                        depth--;
                        if (depth == 0)
                        {
                            static const std::set<std::string> conditions{"if", "while", "for", "foreach", "switch"};
                            if (k && conditions.count(tokens[static_cast<size_t>(k - 1)].Low()))
                                method = false;
                            break;
                        }
                    }
                }
            }
            refs.push_back({i, t.text, std::nullopt, std::nullopt, method, false});
        }
        return refs;
    }

    std::pair<std::string, int> RepairBlockComments(const std::string_view text)
    {
        const auto s = unicode::Decode(text);
        const size_t n = s.size();
        std::u32string out;
        out.reserve(n + 8);
        int count = 0;
        size_t pos = 0;
        while (pos < n)
        {
            if (s[pos] == '/' && pos + 1 < n && s[pos + 1] == '/')
            {
                size_t end = pos + 2;
                while (end < n && s[end] != '\n')
                    end++;
                out.append(s, pos, end - pos);
                pos = end;
                continue;
            }
            if (s[pos] == '/' && pos + 1 < n && s[pos + 1] == '*')
            {
                if (const auto close = s.find(U"*/", pos + 2); close != std::u32string::npos)
                {
                    out.append(s, pos, close + 2 - pos);
                    pos = close + 2;
                    continue;
                }
                // broken: the first line "<tabs/spaces>*\" (the backslash ends the match)
                bool repaired = false;
                for (size_t k = pos + 2; k <= n && !repaired; k++)
                {
                    if (s[k - 1] != '\n')
                        continue;
                    size_t j = k;
                    while (j < n && (s[j] == '\t' || s[j] == ' '))
                        j++;
                    if (j + 1 >= n || s[j] != '*' || s[j + 1] != '\\')
                        continue;
                    size_t after = j + 2;
                    while (after < n && (s[after] == '\t' || s[after] == ' '))
                        after++;
                    const bool ends = after == n || s[after] == '\n' || (s[after] == '\r' && after + 1 < n && s[after + 1] == '\n');
                    if (!ends)
                        continue;
                    out.append(s, pos, j + 1 - pos); // through the '*'
                    out += U'/';
                    pos = j + 2;
                    count++;
                    repaired = true;
                }
                if (repaired)
                    continue;
                out.append(s, pos, n - pos);
                out += U"\n*/\n";
                count++;
                pos = n;
                continue;
            }
            out += s[pos++];
        }
        return {unicode::Encode(out), count};
    }
} // namespace w2b::gsc

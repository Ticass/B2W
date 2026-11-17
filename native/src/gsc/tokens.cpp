// Port of gsc._TOKEN_RE / gsc.tokenize: the alternatives, their order and
// the regex backtracking results are reproduced by hand on code points.
#include "tokens.h"

#include "core/encoding.h"
#include "core/unicode.h"

#include <array>

namespace w2b::gsc
{
    namespace
    {
        using unicode::IsDigit;
        using unicode::IsSpace;
        using unicode::IsWord;
        constexpr size_t NPOS = std::u32string::npos;

        bool AsciiLetter(const char32_t c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        // One repetition of the ws group at pos: end, or NPOS
        size_t WsUnit(const std::u32string& s, const size_t pos)
        {
            const size_t n = s.size();
            if (IsSpace(s[pos]))
            {
                size_t end = pos;
                while (end < n && IsSpace(s[end]))
                    end++;
                return end;
            }
            if (s[pos] != '/' || pos + 1 >= n)
                return NPOS;
            if (s[pos + 1] == '/')
            {
                size_t end = pos + 2;
                while (end < n && s[end] != '\n')
                    end++;
                return end;
            }
            if (s[pos + 1] != '*')
                return NPOS;
            // /\*.*?\*/ : the first */ after the opener
            const auto close = s.find(U"*/", pos + 2);
            if (close != NPOS)
                return close + 2;
            // /\*.*?(?m:^[\t ]*\*\\[\t ]*(?:\r?\n|$)) : the first line that is "*\"
            for (size_t k = pos + 2; k <= n; k++)
            {
                if (k != 0 && s[k - 1] != '\n')
                    continue;
                size_t j = k;
                while (j < n && (s[j] == '\t' || s[j] == ' '))
                    j++;
                if (j + 1 >= n || s[j] != '*' || s[j + 1] != '\\')
                    continue;
                j += 2;
                while (j < n && (s[j] == '\t' || s[j] == ' '))
                    j++;
                if (j < n && s[j] == '\n')
                    return j + 1;
                if (j + 1 < n && s[j] == '\r' && s[j + 1] == '\n')
                    return j + 2;
                if (j == n)
                    return j;
            }
            // /\*(?:(?!\*/).)*\Z : no */ follows, so the rest of the file
            return n;
        }

        size_t Ws(const std::u32string& s, size_t pos)
        {
            const size_t start = pos;
            while (pos < s.size())
            {
                const auto end = WsUnit(s, pos);
                if (end == NPOS)
                    break;
                pos = end;
            }
            return pos > start ? pos : NPOS;
        }

        bool StartsWith(const std::u32string& s, const size_t pos, const std::u32string_view word)
        {
            return s.compare(pos, word.size(), word) == 0;
        }

        size_t Directive(const std::u32string& s, const size_t pos)
        {
            if (s[pos] != '#')
                return NPOS;
            for (const std::u32string_view word : {U"include", U"using_animtree", U"insert", U"define"})
            {
                if (!StartsWith(s, pos + 1, word))
                    continue;
                size_t end = pos + 1 + word.size();
                if (end < s.size() && IsWord(s[end]))
                    continue; // \b
                while (end < s.size() && s[end] != ';' && s[end] != '\n')
                    end++;
                if (end < s.size() && s[end] == ';')
                    end++;
                return end;
            }
            return NPOS;
        }

        size_t String(const std::u32string& s, size_t pos)
        {
            if (s[pos] == '&' || s[pos] == '#')
                pos++;
            if (pos >= s.size() || s[pos] != '"')
                return NPOS;
            pos++;
            while (pos < s.size())
            {
                if (s[pos] == '\\' && pos + 1 < s.size())
                    pos += 2;
                else if (s[pos] == '"')
                    return pos + 1;
                else if (s[pos] == '\\' || s[pos] == '\n')
                    return NPOS;
                else
                    pos++;
            }
            return NPOS;
        }

        size_t Word(const std::u32string& s, size_t pos)
        {
            if (pos >= s.size() || !AsciiLetter(s[pos]))
                return NPOS;
            pos++;
            while (pos < s.size() && IsWord(s[pos]))
                pos++;
            return pos;
        }

        size_t PathToken(const std::u32string& s, const size_t pos)
        {
            size_t end = Word(s, pos);
            if (end == NPOS)
                return NPOS;
            bool segment = false;
            while (end < s.size() && s[end] == '\\')
            {
                const auto next = Word(s, end + 1);
                if (next == NPOS)
                    break;
                end = next;
                segment = true;
            }
            return segment ? end : NPOS;
        }

        size_t Number(const std::u32string& s, const size_t pos)
        {
            const size_t n = s.size();
            size_t end = pos;
            while (end < n && IsDigit(s[end]))
                end++;
            if (end > pos)
            {
                if (end < n && s[end] == '.')
                {
                    end++;
                    while (end < n && IsDigit(s[end]))
                        end++;
                }
            }
            else
            {
                if (pos >= n || s[pos] != '.' || pos + 1 >= n || !IsDigit(s[pos + 1]))
                    return NPOS;
                end = pos + 1;
                while (end < n && IsDigit(s[end]))
                    end++;
            }
            if (end < n && (s[end] == 'e' || s[end] == 'E'))
            {
                size_t e = end + 1;
                if (e < n && (s[e] == '-' || s[e] == '+'))
                    e++;
                const size_t digits = e;
                while (e < n && IsDigit(s[e]))
                    e++;
                if (e > digits)
                    end = e;
            }
            return end;
        }

        size_t Punct(const std::u32string& s, const size_t pos)
        {
            static constexpr std::array<std::u32string_view, 25> twos{U"::", U"[[", U"]]", U"++", U"--", U"&&", U"||", U"==", U"!=",
                                                                      U"<=", U">=", U"+=", U"-=", U"*=", U"/=", U"%=", U"&=", U"|=",
                                                                      U"^=", U"<<", U">>", U"/#", U"#/", U"", U""};
            for (const auto two : twos)
                if (!two.empty() && StartsWith(s, pos, two))
                    return pos + 2;
            static constexpr std::u32string_view singles = U"-+*/%<>=!&|^~?:;,.(){}[]#@";
            return singles.find(s[pos]) != std::u32string_view::npos ? pos + 1 : NPOS;
        }

        int Newlines(const std::string& text)
        {
            int count = 0;
            for (const char c : text)
                count += c == '\n';
            return count;
        }
    } // namespace

    const char* KindName(const Kind kind)
    {
        switch (kind)
        {
        case Kind::Ident:
            return "ident";
        case Kind::Path:
            return "path";
        case Kind::String:
            return "string";
        case Kind::Number:
            return "number";
        case Kind::Punct:
            return "punct";
        case Kind::Directive:
            return "directive";
        default:
            return "eof";
        }
    }

    std::string Lower(const std::string_view text)
    {
        std::string out(text);
        for (auto& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    std::string Token::Low() const
    {
        return Lower(text);
    }

    std::string Repr(const std::string_view text)
    {
        const auto s = unicode::Decode(text);
        const bool single = s.find(U'\'') == NPOS || s.find(U'"') != NPOS;
        const char quote = single ? '\'' : '"';
        std::string out(1, quote);
        for (const auto c : s)
        {
            if (c == '\\' || c == static_cast<char32_t>(quote))
            {
                out += '\\';
                out += static_cast<char>(c);
            }
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0))
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\x%02x", static_cast<unsigned>(c));
                out += buffer;
            }
            else
                encoding::AppendUtf8(out, static_cast<unsigned>(c));
        }
        return out + quote;
    }

    std::vector<Token> Tokenize(const std::string_view source)
    {
        const auto s = unicode::Decode(source);
        std::vector<Token> tokens;
        size_t pos = !s.empty() && s[0] == 0xFEFF ? 1 : 0; // a BOM is not script text
        int line = 1;
        std::u32string pre;
        while (pos < s.size())
        {
            if (const auto end = Ws(s, pos); end != NPOS)
            {
                pre.append(s, pos, end - pos);
                pos = end;
                continue;
            }
            Kind kind;
            size_t end;
            if ((end = Directive(s, pos)) != NPOS)
                kind = Kind::Directive;
            else if ((end = String(s, pos)) != NPOS)
                kind = Kind::String;
            else if ((end = PathToken(s, pos)) != NPOS)
                kind = Kind::Path;
            else if ((end = Number(s, pos)) != NPOS)
                kind = Kind::Number;
            else if ((end = Word(s, pos)) != NPOS)
                kind = Kind::Ident;
            else if ((end = Punct(s, pos)) != NPOS)
                kind = Kind::Punct;
            else
            {
                const auto preText = unicode::Encode(pre);
                throw GscSyntaxError("line " + std::to_string(line + Newlines(preText)) + ": cannot tokenize "
                                     + Repr(unicode::Encode(s.substr(pos, 20))));
            }
            Token token{kind, unicode::Encode(s.substr(pos, end - pos)), unicode::Encode(pre), 0};
            line += Newlines(token.pre);
            token.line = line;
            line += Newlines(token.text);
            tokens.push_back(std::move(token));
            pre.clear();
            pos = end;
        }
        tokens.push_back(Token{Kind::Eof, "", unicode::Encode(pre), line});
        return tokens;
    }

    std::string Emit(const std::vector<Token>& tokens)
    {
        std::string out;
        for (const auto& token : tokens)
        {
            out += token.pre;
            out += token.text;
        }
        return out;
    }
} // namespace w2b::gsc

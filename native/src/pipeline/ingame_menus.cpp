// WaW's in-game menus for T6 LUI (see ingame_menus.h).
#include "ingame_menus.h"

#include "core/regex.h"
#include "core/unicode.h"
#include "csvtable.h"
#include "extract/cache.h"
#include "modutil.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace w2b::pipeline
{
    using util::Json;

    namespace
    {
        const std::string ROOT_MENU = "pausedmenu";
        const std::vector<std::string> MENULISTS{"ui/ingame.txt", "ui/hud.txt"};

        struct MenuSourceError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        using Text = std::u32string;

        std::string U8(const Text& t)
        {
            return unicode::Encode(t);
        }

        Text U32(const std::string& s)
        {
            return unicode::Decode(s);
        }

        // str.lower over Latin-1 (the sources are read as Latin-1)
        char32_t LowerChar(const char32_t c)
        {
            if ((c >= U'A' && c <= U'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
                return c + 32;
            return c;
        }

        std::string LowerU(const std::string& s)
        {
            auto t = U32(s);
            for (auto& c : t)
                c = LowerChar(c);
            return U8(t);
        }

        bool IdentStart(const char32_t c)
        {
            return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || c == U'_';
        }

        // _IDENT.match(text, i): [A-Za-z_]\w*, the end index (i when no match)
        size_t IdentEnd(const Text& text, const size_t i)
        {
            if (i >= text.size() || !IdentStart(text[i]))
                return i;
            size_t j = i + 1;
            while (j < text.size() && unicode::IsWord(text[j]))
                j++;
            return j;
        }

        Text Strip(const Text& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && unicode::IsSpace(s[a]))
                a++;
            while (b > a && unicode::IsSpace(s[b - 1]))
                b--;
            return s.substr(a, b - a);
        }

        Text RStrip(const Text& s)
        {
            size_t b = s.size();
            while (b > 0 && unicode::IsSpace(s[b - 1]))
                b--;
            return s.substr(0, b);
        }

        Text StripChars(const Text& s, const Text& chars)
        {
            size_t a = 0, b = s.size();
            while (a < b && chars.find(s[a]) != Text::npos)
                a++;
            while (b > a && chars.find(s[b - 1]) != Text::npos)
                b--;
            return s.substr(a, b - a);
        }

        std::string StripQuotes(const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && s[a] == '"')
                a++;
            while (b > a && s[b - 1] == '"')
                b--;
            return s.substr(a, b - a);
        }

        // str.split(): whitespace runs
        std::vector<Text> SplitWords(const Text& s)
        {
            std::vector<Text> out;
            Text cur;
            for (const auto c : s)
                if (unicode::IsSpace(c))
                {
                    if (!cur.empty())
                        out.push_back(cur), cur.clear();
                }
                else
                    cur += c;
            if (!cur.empty())
                out.push_back(cur);
            return out;
        }

        size_t Count(const Text& s, const char32_t c, const size_t from, const size_t to)
        {
            return static_cast<size_t>(std::count(s.begin() + static_cast<long long>(from), s.begin() + static_cast<long long>(to), c));
        }

        // ---------------------------------------------------------------- Python numbers

        // An int or a float, with Python's arithmetic (the two eval()s)
        struct Num
        {
            bool isInt = true;
            long long i = 0;
            double f = 0;
            double D() const
            {
                return isInt ? static_cast<double>(i) : f;
            }
            bool Truthy() const
            {
                return isInt ? i != 0 : f != 0;
            }
        };

        struct EvalError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        Num Int(const long long v)
        {
            return {true, v, 0};
        }

        Num Float(const double v)
        {
            return {false, 0, v};
        }

        long long FloorDiv(const long long a, const long long b)
        {
            long long q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0)))
                q--;
            return q;
        }

        Num Arith(const std::string& op, const Num& a, const Num& b)
        {
            const bool ints = a.isInt && b.isInt;
            if (op == "+")
                return ints ? Int(a.i + b.i) : Float(a.D() + b.D());
            if (op == "-")
                return ints ? Int(a.i - b.i) : Float(a.D() - b.D());
            if (op == "*")
                return ints ? Int(a.i * b.i) : Float(a.D() * b.D());
            if (op == "/")
            {
                if (b.D() == 0)
                    throw EvalError("division by zero");
                return Float(a.D() / b.D());
            }
            if (op == "//")
            {
                if (b.D() == 0)
                    throw EvalError("division by zero");
                return ints ? Int(FloorDiv(a.i, b.i)) : Float(std::floor(a.D() / b.D()));
            }
            if (op == "%")
            {
                if (b.D() == 0)
                    throw EvalError("modulo by zero");
                if (ints)
                    return Int(a.i - FloorDiv(a.i, b.i) * b.i);
                double m = std::fmod(a.D(), b.D());
                if (m != 0 && ((m < 0) != (b.D() < 0)))
                    m += b.D();
                return Float(m);
            }
            if (op == "**")
            {
                if (ints && b.i >= 0)
                {
                    long long r = 1;
                    for (long long k = 0; k < b.i; k++)
                        r *= a.i;
                    return Int(r);
                }
                return Float(std::pow(a.D(), b.D()));
            }
            if (ints)
            {
                if (op == "&")
                    return Int(a.i & b.i);
                if (op == "|")
                    return Int(a.i | b.i);
                if (op == "^")
                    return Int(a.i ^ b.i);
                if (op == "<<")
                    return Int(a.i << b.i);
                if (op == ">>")
                    return Int(a.i >> b.i);
            }
            throw EvalError("unsupported operand");
        }

        bool Compare(const std::string& op, const Num& a, const Num& b)
        {
            const double x = a.D(), y = b.D();
            if (a.isInt && b.isInt)
            {
                if (op == "<")
                    return a.i < b.i;
                if (op == ">")
                    return a.i > b.i;
                if (op == "<=")
                    return a.i <= b.i;
                if (op == ">=")
                    return a.i >= b.i;
                if (op == "==")
                    return a.i == b.i;
                return a.i != b.i;
            }
            if (op == "<")
                return x < y;
            if (op == ">")
                return x > y;
            if (op == "<=")
                return x <= y;
            if (op == ">=")
                return x >= y;
            if (op == "==")
                return x == y;
            return x != y;
        }

        // eval() of a Python expression over numbers: arithmetic, comparisons,
        // not/and/or. Anything else is a SyntaxError (EvalError).
        class Evaluator
        {
        public:
            explicit Evaluator(const std::string& source)
            {
                Lex(source);
            }

            Num Run()
            {
                auto v = Or(true);
                if (pos_ != tokens_.size())
                    throw EvalError("invalid syntax");
                return v;
            }

        private:
            struct Token
            {
                enum
                {
                    Number,
                    Op,
                    Word
                } kind;
                std::string text;
                Num value;
            };

            void Lex(const std::string& s)
            {
                size_t i = 0;
                while (i < s.size())
                {
                    const char c = s[i];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
                    {
                        i++;
                        continue;
                    }
                    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9'))
                    {
                        size_t j = i;
                        while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                            j++;
                        bool isFloat = false;
                        if (j < s.size() && s[j] == '.')
                        {
                            isFloat = true;
                            j++;
                            while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                                j++;
                        }
                        if (j < s.size() && (std::isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.'))
                            throw EvalError("invalid syntax");
                        const auto text = s.substr(i, j - i);
                        Token t{Token::Number, text, {}};
                        if (isFloat)
                            t.value = Float(std::strtod(text.c_str(), nullptr));
                        else
                        {
                            // Python rejects leading zeros on a non-zero integer
                            if (text.size() > 1 && text[0] == '0' && text.find_first_not_of('0') != std::string::npos)
                                throw EvalError("leading zeros");
                            t.value = Int(std::stoll(text));
                        }
                        tokens_.push_back(t);
                        i = j;
                        continue;
                    }
                    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                    {
                        size_t j = i;
                        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                            j++;
                        const auto word = s.substr(i, j - i);
                        if (word != "and" && word != "or" && word != "not")
                            throw EvalError("name is not defined");
                        tokens_.push_back({Token::Word, word, {}});
                        i = j;
                        continue;
                    }
                    static const std::vector<std::string> ops{"**", "//", "<<", ">>", "<=", ">=", "==", "!=", "+", "-", "*", "/", "%",
                                                              "<",  ">",  "(",  ")",  "&",  "|",  "^",  "~"};
                    bool matched = false;
                    for (const auto& op : ops)
                        if (s.compare(i, op.size(), op) == 0)
                        {
                            tokens_.push_back({Token::Op, op, {}});
                            i += op.size();
                            matched = true;
                            break;
                        }
                    if (!matched)
                        throw EvalError("invalid syntax");
                }
            }

            bool At(const std::string& text) const
            {
                return pos_ < tokens_.size() && tokens_[pos_].kind != Token::Number && tokens_[pos_].text == text;
            }

            // live: false while a short-circuit skips the operand (no errors raised)
            Num Or(const bool live)
            {
                auto v = And(live);
                while (At("or"))
                {
                    pos_++;
                    const bool decided = v.Truthy();
                    auto rhs = And(live && !decided);
                    if (!decided)
                        v = rhs;
                }
                return v;
            }

            Num And(const bool live)
            {
                auto v = Not(live);
                while (At("and"))
                {
                    pos_++;
                    const bool decided = !v.Truthy();
                    auto rhs = Not(live && !decided);
                    if (!decided)
                        v = rhs;
                }
                return v;
            }

            Num Not(const bool live)
            {
                if (At("not"))
                {
                    pos_++;
                    return Int(Not(live).Truthy() ? 0 : 1);
                }
                return Comparison(live);
            }

            Num Comparison(const bool live)
            {
                auto left = BitOr(live);
                bool result = true, chained = false;
                static const std::set<std::string> ops{"<", ">", "<=", ">=", "==", "!="};
                while (pos_ < tokens_.size() && tokens_[pos_].kind == Token::Op && ops.count(tokens_[pos_].text))
                {
                    const auto op = tokens_[pos_++].text;
                    auto right = BitOr(live && result);
                    if (result)
                        result = Compare(op, left, right);
                    left = right;
                    chained = true;
                }
                return chained ? Int(result ? 1 : 0) : left;
            }

            Num Binary(const bool live, const std::vector<std::string>& ops, Num (Evaluator::*next)(bool))
            {
                auto v = (this->*next)(live);
                while (pos_ < tokens_.size() && tokens_[pos_].kind == Token::Op && std::find(ops.begin(), ops.end(), tokens_[pos_].text) != ops.end())
                {
                    const auto op = tokens_[pos_++].text;
                    const auto rhs = (this->*next)(live);
                    if (live)
                        v = Arith(op, v, rhs);
                }
                return v;
            }

            Num BitOr(const bool live)
            {
                return Binary(live, {"|"}, &Evaluator::BitXor);
            }
            Num BitXor(const bool live)
            {
                return Binary(live, {"^"}, &Evaluator::BitAnd);
            }
            Num BitAnd(const bool live)
            {
                return Binary(live, {"&"}, &Evaluator::Shift);
            }
            Num Shift(const bool live)
            {
                return Binary(live, {"<<", ">>"}, &Evaluator::Sum);
            }
            Num Sum(const bool live)
            {
                return Binary(live, {"+", "-"}, &Evaluator::Term);
            }
            Num Term(const bool live)
            {
                return Binary(live, {"*", "/", "//", "%"}, &Evaluator::Factor);
            }

            Num Factor(const bool live)
            {
                if (At("-") || At("+") || At("~"))
                {
                    const auto op = tokens_[pos_++].text;
                    auto v = Factor(live);
                    if (op == "-")
                        return v.isInt ? Int(-v.i) : Float(-v.f);
                    if (op == "~")
                    {
                        if (!v.isInt)
                        {
                            if (live)
                                throw EvalError("bad operand type for unary ~");
                            return v;
                        }
                        return Int(~v.i);
                    }
                    return v;
                }
                return Power(live);
            }

            Num Power(const bool live)
            {
                auto base = Atom(live);
                if (At("**"))
                {
                    pos_++;
                    const auto exponent = Factor(live);
                    if (live)
                        return Arith("**", base, exponent);
                }
                return base;
            }

            Num Atom(const bool live)
            {
                if (pos_ >= tokens_.size())
                    throw EvalError("unexpected EOF");
                const auto& t = tokens_[pos_];
                if (t.kind == Token::Number)
                {
                    pos_++;
                    return t.value;
                }
                if (At("("))
                {
                    pos_++;
                    auto v = Or(live);
                    if (!At(")"))
                        throw EvalError("'(' was never closed");
                    pos_++;
                    return v;
                }
                throw EvalError("invalid syntax");
            }

            std::vector<Token> tokens_;
            size_t pos_ = 0;
        };

        // ---------------------------------------------------------------- preprocessor

        Text StripComments(const Text& text)
        {
            Text out;
            size_t i = 0;
            const size_t n = text.size();
            while (i < n)
            {
                const auto c = text[i];
                if (c == U'"')
                {
                    size_t j = i + 1;
                    while (j < n && text[j] != U'"' && text[j] != U'\n')
                        j++;
                    out += text.substr(i, j + 1 - i);
                    i = j + 1;
                }
                else if (text.compare(i, 2, U"//") == 0)
                {
                    const auto j = text.find(U'\n', i);
                    if (j == Text::npos)
                        break;
                    // A // comment on a continued #define line keeps the continuation.
                    const auto comment = RStrip(text.substr(i, j - i));
                    if (!comment.empty() && comment.back() == U'\\')
                        out += U'\\';
                    i = j;
                }
                else if (text.compare(i, 2, U"/*") == 0)
                {
                    const auto j = text.find(U"*/", i + 2);
                    if (j == Text::npos)
                        break;
                    const auto lines = Count(text, U'\n', i, j);
                    out += lines ? Text(lines, U'\n') : Text(U" ");
                    i = j + 2;
                }
                else
                {
                    out += c;
                    i++;
                }
            }
            return out;
        }

        // _split_args: text[start] is '('
        std::pair<std::vector<Text>, size_t> SplitArgs(const Text& text, const size_t start)
        {
            int depth = 0;
            size_t i = start;
            std::vector<Text> args;
            Text cur;
            while (i < text.size())
            {
                const auto c = text[i];
                if (c == U'"')
                {
                    auto j = text.find(U'"', i + 1);
                    j = j == Text::npos ? text.size() - 1 : j;
                    cur += text.substr(i, j + 1 - i);
                    i = j + 1;
                    continue;
                }
                if (c == U'(')
                {
                    depth++;
                    if (depth > 1)
                        cur += c;
                }
                else if (c == U')')
                {
                    depth--;
                    if (depth == 0)
                    {
                        args.push_back(Strip(cur));
                        return {args, i + 1};
                    }
                    cur += c;
                }
                else if (c == U',' && depth == 1)
                {
                    args.push_back(Strip(cur));
                    cur.clear();
                }
                else
                    cur += c;
                i++;
            }
            throw MenuSourceError("Unterminated macro call");
        }

        // re.sub(r'"[^"]*"', '', line)
        Text DropQuoted(const Text& line)
        {
            Text out;
            size_t i = 0;
            while (i < line.size())
            {
                if (line[i] == U'"')
                {
                    const auto j = line.find(U'"', i + 1);
                    if (j != Text::npos)
                    {
                        i = j + 1;
                        continue;
                    }
                }
                out += line[i++];
            }
            return out;
        }

        long long OpenParens(const Text& line)
        {
            const auto bare = DropQuoted(line);
            return static_cast<long long>(Count(bare, U'(', 0, bare.size())) - static_cast<long long>(Count(bare, U')', 0, bare.size()));
        }

        struct Macro
        {
            std::optional<std::vector<Text>> params;
            Text body;
        };

        Text ReadLatin1(const fs::path& path)
        {
            const auto bytes = mod::Bytes(path);
            Text out;
            out.reserve(bytes.size());
            for (const unsigned char c : bytes)
                out += static_cast<char32_t>(c);
            return out;
        }

        class Preprocessor
        {
        public:
            Preprocessor(fs::path root, const std::map<Text, Text>& defines = {}) : root_(std::move(root))
            {
                for (const auto& [k, v] : defines)
                    macros_[k] = Macro{std::nullopt, v};
            }

            Text Run(const std::string& name)
            {
                return Directives(Read(U32(name)));
            }

        private:
            Text Read(const Text& name)
            {
                const auto path = root_ / files::FromUtf8(U8(name));
                if (!mod::IsFile(path))
                    throw MenuSourceError("WaW menu source missing: " + mod::Str(path));
                return ReadLatin1(path);
            }

            Text Directives(Text text)
            {
                // newlines: \r\n then \r
                Text normalized;
                for (size_t i = 0; i < text.size(); i++)
                {
                    if (text[i] == U'\r')
                    {
                        normalized += U'\n';
                        if (i + 1 < text.size() && text[i + 1] == U'\n')
                            i++;
                        continue;
                    }
                    normalized += text[i];
                }
                text = StripComments(normalized);
                std::vector<Text> joined;
                Text buf;
                size_t start = 0;
                while (true)
                {
                    const auto end = text.find(U'\n', start);
                    const auto line = text.substr(start, end == Text::npos ? Text::npos : end - start);
                    const auto stripped = RStrip(line);
                    if (!stripped.empty() && stripped.back() == U'\\')
                        buf += stripped.substr(0, stripped.size() - 1) + U" ";
                    else
                    {
                        joined.push_back(buf + line);
                        buf.clear();
                    }
                    if (end == Text::npos)
                        break;
                    start = end + 1;
                }
                if (!buf.empty())
                    joined.push_back(buf);
                std::vector<Text> out;
                std::vector<std::pair<bool, bool>> stack;
                Text pending;
                const auto active = [&] { return std::all_of(stack.begin(), stack.end(), [](const auto& s) { return s.first; }); };
                for (auto line : joined)
                {
                    if (!pending.empty())
                    {
                        line = pending + U" " + line;
                        pending.clear();
                    }
                    const auto s = Strip(line);
                    if (s.empty() || s[0] != U'#')
                    {
                        if (active())
                        {
                            if (OpenParens(line) > 0)
                            {
                                pending = line; // a macro call continues on the next line
                                continue;
                            }
                            out.push_back(Expand(line, {}));
                        }
                        continue;
                    }
                    // #\s*(\w*)\s*(.*)
                    size_t k = 1;
                    while (k < s.size() && unicode::IsSpace(s[k]))
                        k++;
                    size_t w = k;
                    while (w < s.size() && unicode::IsWord(s[w]))
                        w++;
                    const auto word = U8(s.substr(k, w - k));
                    while (w < s.size() && unicode::IsSpace(s[w]))
                        w++;
                    const auto rest = Strip(s.substr(w));
                    const auto first = [&] { const auto words = SplitWords(rest); return words.empty() ? Text() : words[0]; };
                    if (word == "ifdef" || word == "ifndef")
                    {
                        const bool cond = macros_.count(first()) == (word == "ifdef");
                        stack.push_back({cond && active(), cond});
                    }
                    else if (word == "if")
                    {
                        const bool cond = Condition(rest);
                        stack.push_back({cond && active(), cond});
                    }
                    else if (word == "else")
                    {
                        if (stack.empty())
                            throw StepError("IndexError", "list index out of range");
                        bool parent = true;
                        for (size_t x = 0; x + 1 < stack.size(); x++)
                            parent = parent && stack[x].first;
                        stack.back().first = parent && !stack.back().second;
                    }
                    else if (word == "endif")
                    {
                        if (stack.empty())
                            throw StepError("IndexError", "pop from empty list");
                        stack.pop_back();
                    }
                    else if (!active())
                        continue;
                    else if (word == "include")
                        out.push_back(Directives(Read(StripChars(rest, U"\"<> "))));
                    else if (word == "define")
                    {
                        const auto end = IdentEnd(rest, 0);
                        if (end == 0)
                            continue;
                        const auto name = rest.substr(0, end);
                        const auto tail = rest.substr(end);
                        if (!tail.empty() && tail[0] == U'(')
                        {
                            const auto close = tail.find(U')');
                            if (close == Text::npos)
                                throw StepError("ValueError", "substring not found");
                            std::vector<Text> params;
                            size_t from = 1;
                            while (true)
                            {
                                const auto comma = tail.find(U',', from);
                                const auto piece = Strip(tail.substr(from, (comma == Text::npos || comma > close ? close : comma) - from));
                                if (!piece.empty())
                                    params.push_back(piece);
                                if (comma == Text::npos || comma > close)
                                    break;
                                from = comma + 1;
                            }
                            macros_[name] = Macro{params, Strip(tail.substr(close + 1))};
                        }
                        else
                            macros_[name] = Macro{std::nullopt, Strip(tail)};
                    }
                    else if (word == "undef")
                        macros_.erase(first());
                }
                Text result;
                for (size_t i = 0; i < out.size(); i++)
                    result += (i ? U"\n" : U"") + out[i];
                return result;
            }

            bool Condition(const Text& expr)
            {
                // defined(X) -> 1/0
                static const regex::Regex defined("defined\\s*\\(\\s*(\\w+)\\s*\\)");
                auto text = defined.Sub([&](const regex::Match& m) { return macros_.count(U32(m[1])) ? std::string("1") : std::string("0"); }, U8(expr));
                auto expanded = U8(Expand(U32(text), {}));
                // && / || -> and / or, ! (not !=) -> not
                std::string s;
                for (size_t i = 0; i < expanded.size(); i++)
                {
                    if (expanded.compare(i, 2, "&&") == 0)
                        s += " and ", i++;
                    else if (expanded.compare(i, 2, "||") == 0)
                        s += " or ", i++;
                    else
                        s += expanded[i];
                }
                std::string t;
                for (size_t i = 0; i < s.size(); i++)
                    if (s[i] == '!' && !(i + 1 < s.size() && s[i + 1] == '='))
                        t += " not ";
                    else
                        t += s[i];
                // every other identifier is 0
                static const regex::Regex word("[A-Za-z_]\\w*");
                const auto final = word.Sub([](const regex::Match& m) { const auto w = m[0]; return w == "and" || w == "or" || w == "not" ? w : std::string("0"); }, t);
                try
                {
                    return Evaluator(final).Run().Truthy();
                }
                catch (const std::exception&)
                {
                    return false;
                }
            }

            Text Expand(const Text& text, const std::set<Text>& hide)
            {
                Text out;
                size_t i = 0;
                const size_t n = text.size();
                while (i < n)
                {
                    const auto c = text[i];
                    if (c == U'"')
                    {
                        auto j = text.find(U'"', i + 1);
                        j = j == Text::npos ? n - 1 : j;
                        out += text.substr(i, j + 1 - i);
                        i = j + 1;
                        continue;
                    }
                    const auto end = IdentEnd(text, i);
                    if (end == i)
                    {
                        out += c;
                        i++;
                        continue;
                    }
                    const auto name = text.substr(i, end - i);
                    const auto found = macros_.find(name);
                    if (found == macros_.end() || hide.count(name))
                    {
                        out += name;
                        i = end;
                        continue;
                    }
                    const auto macro = found->second; // a copy: expansion may redefine nothing, but stays safe
                    auto inner = hide;
                    inner.insert(name);
                    if (!macro.params)
                    {
                        out += Expand(macro.body, inner);
                        i = end;
                        continue;
                    }
                    size_t j = end;
                    while (j < n && (text[j] == U' ' || text[j] == U'\t' || text[j] == U'\n'))
                        j++;
                    if (j >= n || text[j] != U'(')
                    {
                        out += name;
                        i = end;
                        continue;
                    }
                    auto [args, after] = SplitArgs(text, j);
                    for (auto& a : args)
                        a = Expand(a, hide);
                    std::map<Text, Text> values;
                    // dict(zip(params, args + [''] * ...)): a repeated name keeps the last
                    for (size_t p = 0; p < macro.params->size(); p++)
                        values[(*macro.params)[p]] = p < args.size() ? args[p] : Text();
                    // re.sub(r'"[^"]*"|[A-Za-z_]\w*', ...) over the body
                    Text body;
                    size_t b = 0;
                    const auto& src = macro.body;
                    while (b < src.size())
                    {
                        if (src[b] == U'"')
                        {
                            const auto q = src.find(U'"', b + 1);
                            if (q != Text::npos)
                            {
                                body += src.substr(b, q + 1 - b);
                                b = q + 1;
                                continue;
                            }
                        }
                        const auto e = IdentEnd(src, b);
                        if (e > b)
                        {
                            const auto ident = src.substr(b, e - b);
                            const auto v = values.find(ident);
                            body += v == values.end() ? ident : v->second;
                            b = e;
                            continue;
                        }
                        body += src[b++];
                    }
                    out += Expand(body, inner);
                    i = after;
                }
                return out;
            }

            fs::path root_;
            std::map<Text, Macro> macros_;
        };

        // ---------------------------------------------------------------- parser

        using Tokens = std::vector<std::string>;
        using Commands = std::vector<Tokens>;

        Tokens Tokenize(const std::string& text)
        {
            static const regex::Regex token(R"re("(?:[^"\\]|\\.)*"|[{}();,]|\|\||&&|[!=<>]=|[-+*/%!<>()=?:]|[^\s{}();,"!=<>+*/%?:-]+)re");
            Tokens out;
            for (const auto& m : token.FindAll(text))
                out.push_back(m[0]);
            return out;
        }

        const std::vector<std::string> SCRIPT_KEYS{"onopen", "onclose", "onesc", "onfocus", "leavefocus", "action", "mouseenter", "mouseexit",
                                                   "mouseentertext", "mouseexittext"};
        const std::map<std::string, int> VECTOR_ARITY{{"origin", 2},     {"forecolor", 4},  {"backcolor", 4},    {"bordercolor", 4},
                                                      {"outlinecolor", 4}, {"focuscolor", 4}, {"disablecolor", 4}, {"glowcolor", 4}};
        const std::map<std::string, std::optional<int>> COMMAND_ARGS{
            {"fadein", 1},
            {"fadeout", 1},
            {"show", 1},
            {"hide", 1},
            {"showmenu", 1},
            {"hidemenu", 1},
            {"setcolor", 5},
            {"open", 1},
            {"close", 1},
            {"closeforallplayers", 1},
            {"ingameopen", 1},
            {"ingameclose", 1},
            {"setbackground", 2},
            {"setitemcolor", 6},
            {"focusfirst", 0},
            {"setfocus", 1},
            {"setfocusbydvar", 1},
            {"setdvar", 2},
            {"exec", 1},
            {"execnow", 1},
            {"execondvarstringvalue", 3},
            {"execondvarintvalue", 3},
            {"execondvarfloatvalue", 3},
            {"execnowondvarstringvalue", 3},
            {"execnowondvarintvalue", 3},
            {"execnowondvarfloatvalue", 3},
            {"play", 1},
            {"scriptmenuresponse", 1},
            {"scriptmenurespondondvarstringvalue", 3},
            {"scriptmenurespondondvarintvalue", 3},
            {"scriptmenurespondondvarfloatvalue", 3},
            {"uiscript", std::nullopt},
            {"setlocalvarint", 2},
            {"setlocalvarstring", 2},
            {"setlocalvarfloat", 2},
            {"setlocalvarbool", 2},
            {"nosavehide", 1},
            {"saveavailablehide", 1},
            {"profilehide", 1},
            {"setsaveexeconsuccess", 1},
            {"closeingame", 1},
            {"stopmusic", 0},
            {"setfromdvar", 2},
            {"setmenuowner", 1}};
        const std::set<std::string> FUNCTIONS{"text", "visible", "rect", "name"};
        const std::set<std::string> KEYWORDS = []
        {
            std::set<std::string> k{"name",          "rect",          "style",       "background",   "forecolor",   "backcolor",     "bordercolor",
                                    "border",        "bordersize",    "text",        "textfont",     "textscale",   "textstyle",     "textalign",
                                    "textalignx",    "textaligny",    "visible",     "decoration",   "type",        "exp",           "ownerdraw",
                                    "itemdef",       "fullscreen",    "focuscolor",  "blurworld",    "allowedbinding", "origin",     "group",
                                    "dvar",          "dvartest",      "showdvar",    "hidedvar",     "textfile",    "feeder",        "elementwidth",
                                    "elementheight", "elementtype",   "disablecolor", "outlinecolor", "popup",      "legacysplitscreenscale",
                                    "execkey",       "execkeyint",    "soundloop",   "disabled",     "ownerdrawflag", "autowrapped", "textsavegame",
                                    "outofbounds",   "menudef",       "maxchars",    "notselectable", "noscrollbars", "usepaging",   "columns"};
            k.insert(SCRIPT_KEYS.begin(), SCRIPT_KEYS.end());
            return k;
        }();

        bool IsKeyword(const std::optional<std::string>& tok)
        {
            return tok && KEYWORDS.count(LowerU(*tok));
        }

        Commands SplitCommands(const Commands& commands)
        {
            Commands out;
            for (const auto& command : commands)
            {
                size_t i = 0;
                while (i < command.size())
                {
                    const auto it = COMMAND_ARGS.find(LowerU(command[i]));
                    if (it == COMMAND_ARGS.end() || !it->second)
                    {
                        out.emplace_back(command.begin() + static_cast<long long>(i), command.end());
                        break;
                    }
                    const auto count = static_cast<size_t>(*it->second);
                    out.emplace_back(command.begin() + static_cast<long long>(i), command.begin() + static_cast<long long>(std::min(command.size(), i + 1 + count)));
                    i += 1 + count;
                }
            }
            return out;
        }

        bool AllDigits(const std::string& s)
        {
            const auto t = U32(s);
            return !t.empty() && std::all_of(t.begin(), t.end(), [](const char32_t c) { return unicode::IsDigit(c); });
        }

        class Parser
        {
        public:
            explicit Parser(Tokens tokens) : t_(std::move(tokens))
            {
            }

            std::optional<std::string> Peek(const size_t k = 0) const
            {
                return i_ + k < t_.size() ? std::optional(t_[i_ + k]) : std::nullopt;
            }

            std::string Take()
            {
                if (i_ >= t_.size())
                    throw StepError("IndexError", "list index out of range");
                return t_[i_++];
            }

            void Expect(const std::string& value)
            {
                const auto tok = Take();
                if (tok != value)
                    throw MenuSourceError("Expected " + util::StringRepr(value) + ", found " + util::StringRepr(tok) + " near token " + std::to_string(i_));
            }

            Json Block()
            {
                Expect("{");
                Commands commands;
                Tokens cur;
                int depth = 1;
                while (true)
                {
                    const auto tok = Take();
                    if (tok == "{")
                        depth++;
                    else if (tok == "}")
                    {
                        depth--;
                        if (depth == 0)
                            break;
                    }
                    if (tok == ";" && depth == 1)
                    {
                        if (!cur.empty())
                            commands.push_back(cur);
                        cur.clear();
                    }
                    else if ((tok != "{" && tok != "}") || depth > 1)
                        cur.push_back(tok);
                }
                if (!cur.empty())
                    commands.push_back(cur);
                return Json(SplitCommands(commands));
            }

            Json Expression()
            {
                // after one of these an operand follows; after anything else the operand is complete
                static const std::set<std::string> EXPECTS_OPERAND{"(", ",", "!", "||", "&&", "==", "!=", "<", ">", "<=", ">=", "+", "-", "*", "/", "%"};
                int depth = 0;
                Tokens out;
                while (Peek())
                {
                    const auto tok = *Peek();
                    // a keyword ends the expression, unless it is a function an operator is waiting for:
                    // "exp forecolor A(x)" then "text ..." on the next line is the item's text (UGX objective_info.menu)
                    if (depth == 0 && (tok == ";" || tok == "}" ||
                                       (!out.empty() && IsKeyword(tok) && (!FUNCTIONS.count(LowerU(tok)) || !EXPECTS_OPERAND.count(out.back())))))
                        break;
                    Take();
                    depth += (tok == "(") - (tok == ")");
                    out.push_back(tok);
                }
                if (Peek() == std::optional<std::string>(";"))
                    Take();
                return Json(out);
            }

            std::string Number()
            {
                const auto tok = Take();
                if (tok == "-")
                    return "-" + Number();
                if (tok != "(")
                    return tok;
                int depth = 1;
                std::string out = "(";
                while (depth)
                {
                    const auto next = Take();
                    depth += (next == "(") - (next == ")");
                    out += " " + next;
                }
                return out;
            }

            Json ValueLine()
            {
                Tokens out;
                while (Peek() && *Peek() != "}" && *Peek() != ";" && !(IsKeyword(Peek()) && !out.empty()))
                    out.push_back(Take());
                if (Peek() == std::optional<std::string>(";"))
                    Take();
                return Json(out);
            }

            Json Definition()
            {
                Expect("{");
                Json d{{"items", Json::array()}};
                while (Peek() != std::optional<std::string>("}"))
                {
                    const auto key = Take();
                    const auto low = LowerU(key);
                    if (low == ";")
                        continue;
                    if (low == "itemdef")
                        d["items"].push_back(Definition());
                    else if (std::find(SCRIPT_KEYS.begin(), SCRIPT_KEYS.end(), low) != SCRIPT_KEYS.end())
                        d[low] = Block();
                    else if ((low == "visible" || low == "disabled") && Peek() && LowerU(*Peek()) == "when")
                    {
                        Take();
                        d[low] = Expression();
                    }
                    else if (low == "exp")
                    {
                        auto prop = LowerU(Take());
                        if (prop == "rect" || prop == "forecolor" || prop == "backcolor")
                            prop += " " + LowerU(Take());
                        if (Peek() && LowerU(*Peek()) == "when")
                            Take();
                        if (!d.contains("exp"))
                            d["exp"] = Json::object();
                        d["exp"][prop] = Expression();
                    }
                    else if (low == "decoration")
                        d["decoration"] = true;
                    else if (VECTOR_ARITY.count(low))
                    {
                        Json values = Json::array();
                        for (int k = 0; k < VECTOR_ARITY.at(low); k++)
                            values.push_back(Number());
                        d[low] = values;
                    }
                    else if (low == "rect")
                    {
                        Json values = Json::array();
                        for (int k = 0; k < 4; k++)
                            values.push_back(Number());
                        while (Peek() && AllDigits(*Peek()) && values.size() < 6)
                            values.push_back(Take());
                        d["rect"] = values;
                    }
                    else
                        d[low] = ValueLine();
                }
                Expect("}");
                return d;
            }

        private:
            Tokens t_;
            size_t i_ = 0;
        };

        std::vector<Json> Parse(const std::string& text)
        {
            Parser p(Tokenize(text));
            std::vector<Json> menus;
            while (p.Peek())
                if (LowerU(p.Take()) == "menudef")
                    menus.push_back(p.Definition());
            return menus;
        }

        std::string JoinTokens(const Json& tokens, const std::string& sep)
        {
            std::string out;
            bool first = true;
            for (const auto& t : tokens)
            {
                out += (first ? "" : sep) + t.get<std::string>();
                first = false;
            }
            return out;
        }

        std::string MenuName(const Json& menu)
        {
            return StripQuotes(menu.contains("name") ? JoinTokens(menu["name"], "") : std::string());
        }

        // ---------------------------------------------------------------- source set

        std::map<std::string, Json> LoadSources(const fs::path& raw)
        {
            static const regex::Regex loadMenu("loadMenu\\s*\\{\\s*\"([^\"]+)\"");
            std::map<std::string, Json> menus;
            for (const auto& menulist : MENULISTS)
            {
                if (!mod::IsFile(raw / files::FromUtf8(menulist)))
                    throw MenuSourceError("WaW menulist missing: " + mod::Str(raw / files::FromUtf8(menulist)));
                const auto listing = U8(Preprocessor(raw).Run(menulist));
                for (const auto& m : loadMenu.FindAll(listing))
                {
                    const auto& file = m[0];
                    if (!mod::IsFile(raw / files::FromUtf8(file)))
                        continue; // listed but not shipped in raw/: not an in-game menu we draw
                    for (auto& menu : Parse(U8(Preprocessor(raw, {{U"PC", U"1"}}).Run(file))))
                        menus[LowerU(MenuName(menu))] = menu;
                }
            }
            return menus;
        }

        void References(const Json& d, std::set<std::string>& out)
        {
            for (const auto& key : SCRIPT_KEYS)
                if (d.contains(key))
                    for (const auto& command : d[key])
                        if (!command.empty() && command.size() > 1)
                        {
                            const auto head = LowerU(command[0].get<std::string>());
                            if (head == "open" || head == "showmenu")
                                out.insert(LowerU(StripQuotes(command[1].get<std::string>())));
                        }
            if (d.contains("items"))
                for (const auto& item : d["items"])
                    References(item, out);
        }

        std::vector<std::string> Closure(const std::map<std::string, Json>& menus, const std::set<std::string>& stop, std::vector<std::string> todo)
        {
            std::vector<std::string> seen;
            while (!todo.empty())
            {
                const auto name = todo.front();
                todo.erase(todo.begin());
                if (std::find(seen.begin(), seen.end(), name) != seen.end() || !menus.count(name) || stop.count(name))
                    continue;
                seen.push_back(name);
                std::set<std::string> refs;
                References(menus.at(name), refs);
                todo.insert(todo.end(), refs.begin(), refs.end());
            }
            return seen;
        }

        // ---------------------------------------------------------------- expressions

        const std::map<std::string, int> PREC{{"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<", 4}, {">", 4}, {"<=", 4},
                                              {">=", 4}, {"+", 5},  {"-", 5},  {"*", 6},  {"/", 6},  {"%", 6}};

        // _q: re.sub(r'[%\s,"|;=]', '%%%02X')
        std::string Q(const std::string& text)
        {
            Text out;
            for (const auto c : U32(text))
                if (c == U'%' || c == U',' || c == U'"' || c == U'|' || c == U';' || c == U'=' || unicode::IsSpace(c))
                {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "%%%02X", static_cast<unsigned>(c));
                    out += U32(hex);
                }
                else
                    out += c;
            return U8(out);
        }

        std::string Rpn(const Tokens& tokens)
        {
            if (tokens.empty())
                return "";
            static const regex::Regex number("\\d+(\\.\\d*)?|\\.\\d+");
            std::vector<std::string> out;
            size_t pos = 0;
            const auto at = [&](const size_t k) -> const std::string&
            {
                if (k >= tokens.size())
                    throw StepError("IndexError", "list index out of range");
                return tokens[k];
            };
            std::function<void(int)> expr;
            std::function<void()> primary = [&]
            {
                const auto tok = at(pos);
                pos++;
                if (tok == "(")
                {
                    expr(0);
                    pos++; // ')'
                }
                else if (tok == "!")
                {
                    primary();
                    out.push_back("op:!");
                }
                else if (tok == "-")
                {
                    primary();
                    out.push_back("op:neg");
                }
                else if (!tok.empty() && tok[0] == '"')
                    out.push_back("s:" + Q(tok.substr(1, tok.size() >= 2 ? tok.size() - 2 : 0)));
                else if (number.FullMatch(tok))
                    out.push_back("n:" + tok);
                else if (pos < tokens.size() && tokens[pos] == "(")
                {
                    pos++;
                    int argc = 0;
                    if (at(pos) != ")")
                        while (true)
                        {
                            expr(0);
                            argc++;
                            if (at(pos) == ",")
                            {
                                pos++;
                                continue;
                            }
                            break;
                        }
                    pos++;
                    out.push_back("f:" + LowerU(tok) + ":" + std::to_string(argc));
                }
                else
                    out.push_back("s:" + Q(tok));
            };
            expr = [&](const int level)
            {
                primary();
                while (pos < tokens.size() && PREC.count(tokens[pos]) && PREC.at(tokens[pos]) >= level)
                {
                    const auto op = tokens[pos];
                    pos++;
                    expr(PREC.at(op) + 1);
                    out.push_back("op:" + op);
                }
            };
            expr(0);
            if (pos != tokens.size())
            {
                std::string tail;
                for (size_t k = pos; k < tokens.size(); k++)
                    tail += (k > pos ? " " : "") + tokens[k];
                throw MenuSourceError("Unparsed expression tail: " + tail);
            }
            std::string joined;
            for (size_t k = 0; k < out.size(); k++)
                joined += (k ? " " : "") + out[k];
            return joined;
        }

        Tokens AsTokens(const Json& v)
        {
            Tokens out;
            for (const auto& t : v)
                out.push_back(t.get<std::string>());
            return out;
        }

        std::string Script(const Json& commands)
        {
            std::string out;
            bool firstCommand = true;
            for (const auto& command : commands)
            {
                std::string line;
                bool first = true;
                for (const auto& t : command)
                {
                    line += (first ? "" : " ") + Q(StripQuotes(t.get<std::string>()));
                    first = false;
                }
                out += (firstCommand ? "" : ";") + line;
                firstCommand = false;
            }
            return out;
        }

        // _num: a numeric field; the menu compiler folds constant arithmetic
        Num NumberOf(const std::string& raw)
        {
            const auto value = U8(Strip(U32(raw)));
            static const regex::Regex allowed("[\\d.\\s()+*/%-]+");
            if (!allowed.FullMatch(value))
                throw MenuSourceError("Expected a number, found " + util::StringRepr(value));
            Num number;
            try
            {
                number = Evaluator(value).Run();
            }
            catch (const EvalError& error)
            {
                throw StepError("SyntaxError", error.what());
            }
            const double f = number.D();
            if (f == std::floor(f) && std::isfinite(f))
                return Int(static_cast<long long>(f));
            char text[64];
            std::snprintf(text, sizeof text, "%.6f", f);
            return Float(std::strtod(text, nullptr));
        }

        std::string NumText(const Num& n)
        {
            return n.isInt ? std::to_string(n.i) : util::FloatRepr(n.f);
        }

        const std::vector<std::string> ITEM_COLUMNS{"rect",       "style",      "background", "forecolor", "backcolor", "bordercolor", "border",
                                                    "bordersize", "text",       "textfont",   "textscale", "textstyle", "textalign",   "textalignx",
                                                    "textaligny", "visible",    "exp",        "type",      "name",      "onfocus",     "leavefocus",
                                                    "action",     "decoration", "ownerdraw",  "origin",    "autowrapped", "keys"};
        const std::vector<std::string> MENU_COLUMNS{"rect", "onopen", "onclose", "onesc", "blurworld", "items", "visible", "fullscreen", "focuscolor", "keys"};
        const std::set<std::string> SCRIPTS{"onfocus", "leavefocus", "action", "onopen", "onclose", "onesc"};
        const std::set<std::string> NUMBERS{"style",      "border",     "bordersize", "textfont", "textscale", "textstyle", "textalign",
                                            "textalignx", "textaligny", "type",       "ownerdraw", "blurworld", "fullscreen"};
        const std::set<std::string> VECTORS{"rect", "forecolor", "backcolor", "bordercolor", "focuscolor", "origin"};

        std::string Value(const Json& item, const std::string& key)
        {
            const bool has = item.contains(key);
            // a compiled menu (script_menus.cpp) carries its expressions as RPN already
            if (item.contains("compiled") && item["compiled"].contains(key))
                return item["compiled"][key].get<std::string>();
            if (key == "keys")
            {
                // execKey: "<key code>=<script>|..."
                std::string out;
                if (has)
                    for (const auto& handler : item[key])
                        out += (out.empty() ? "" : "|") + std::to_string(handler["key"].get<long long>()) + "=" + Script(handler["commands"]);
                return out;
            }
            if (key == "visible")
                return has ? Rpn(AsTokens(item[key])) : "n:1";
            if (!has)
                return "";
            const auto& v = item[key];
            if (SCRIPTS.count(key))
                return Script(v);
            if (key == "exp")
            {
                std::vector<std::string> props;
                for (const auto& [prop, _] : v.items())
                    props.push_back(prop);
                std::sort(props.begin(), props.end());
                std::string out;
                for (size_t k = 0; k < props.size(); k++)
                    out += (k ? "|" : "") + props[k] + "=" + Rpn(AsTokens(v[props[k]]));
                return out;
            }
            if (key == "decoration" || key == "autowrapped")
                return "1";
            if (VECTORS.count(key))
            {
                std::string out;
                bool first = true;
                for (const auto& x : v)
                {
                    out += (first ? "" : " ") + NumText(NumberOf(x.get<std::string>()));
                    first = false;
                }
                return out;
            }
            std::string text;
            bool first = true;
            for (const auto& t : v)
            {
                text += (first ? "" : " ") + StripQuotes(t.get<std::string>());
                first = false;
            }
            return NUMBERS.count(key) && !text.empty() ? NumText(NumberOf(text)) : text;
        }

        CsvRows Encode(const std::map<std::string, Json>& menus, const std::vector<std::string>& names, const std::map<std::string, std::string>& context)
        {
            CsvRows rows;
            for (const auto& [key, value] : context)
                rows.push_back({key, value});
            for (const auto& name : names)
            {
                const auto& menu = menus.at(name);
                std::vector<std::string> row{"menu:" + name};
                for (const auto& key : MENU_COLUMNS)
                    row.push_back(key == "items" ? std::to_string(menu["items"].size()) : Value(menu, key));
                rows.push_back(row);
                size_t i = 1;
                for (const auto& item : menu["items"])
                {
                    std::vector<std::string> itemRow{"item:" + name + ":" + std::to_string(i++)};
                    for (const auto& key : ITEM_COLUMNS)
                        itemRow.push_back(Value(item, key));
                    rows.push_back(itemRow);
                }
            }
            return rows;
        }

        std::vector<std::string> Literals(const Json& tokens)
        {
            std::vector<std::string> out;
            for (const auto& t : tokens)
            {
                const auto s = t.get<std::string>();
                if (s.size() > 1 && s.front() == '"' && s.back() == '"')
                    out.push_back(s.substr(1, s.size() - 2));
            }
            return out;
        }

        // requirements: the materials, localized strings and literal text the menus draw
        Json Requirements(const std::map<std::string, Json>& menus, const std::vector<std::string>& names, const std::string& mapname)
        {
            std::set<std::string> materials;
            std::map<std::string, std::set<long long>> strings;
            std::map<long long, std::set<char32_t>> literal;
            std::function<void(const Json&)> walk = [&](const Json& d)
            {
                if (d.contains("background") && !d["background"].empty())
                    materials.insert(StripQuotes(JoinTokens(d["background"], " ")));
                const auto exp = d.contains("exp") ? d["exp"] : Json::object();
                if (exp.contains("material"))
                {
                    const auto& tokens = exp["material"];
                    std::string name;
                    for (const auto& l : Literals(tokens))
                        name += l;
                    if (std::any_of(tokens.begin(), tokens.end(), [](const Json& t) { return LowerU(t.get<std::string>()) == "mapname"; }))
                        name += mapname;
                    materials.insert(name);
                }
                long long font = 0;
                if (d.contains("textfont") && !d["textfont"].empty())
                {
                    const auto n = NumberOf(StripQuotes(JoinTokens(d["textfont"], " ")));
                    font = n.isInt ? n.i : static_cast<long long>(n.f);
                }
                std::vector<std::string> texts;
                if (d.contains("text") && !d["text"].empty())
                    texts.push_back(StripQuotes(JoinTokens(d["text"], " ")));
                if (exp.contains("text"))
                    for (const auto& l : Literals(exp["text"]))
                        texts.push_back(l);
                for (const auto& value : texts)
                {
                    if (!value.empty() && value[0] == '@')
                        strings[value.substr(1)].insert(font);
                    else if (!value.empty())
                    {
                        const auto points = U32(value);
                        literal[font].insert(points.begin(), points.end());
                    }
                }
                if (d.contains("items"))
                    for (const auto& item : d["items"])
                        walk(item);
            };
            for (const auto& name : names)
                walk(menus.at(name));
            materials.erase("");
            Json stringsJson = Json::object(), literalJson = Json::object();
            for (const auto& [k, v] : strings)
                stringsJson[k] = std::vector<long long>(v.begin(), v.end());
            for (const auto& [k, v] : literal)
                literalJson[std::to_string(k)] = U8(Text(v.begin(), v.end()));
            return Json{{"materials", std::vector<std::string>(materials.begin(), materials.end())}, {"strings", stringsJson}, {"literal_text", literalJson}};
        }
    } // namespace

    MenuStage StageWawMenus(const fs::path& project, const std::vector<fs::path>& rawRoots, const std::string& mapname,
                            const std::map<std::string, Json>& scriptMenus)
    {
        try
        {
            std::optional<fs::path> raw;
            for (const auto& root : rawRoots)
            {
                if (std::all_of(MENULISTS.begin(), MENULISTS.end(), [&](const std::string& m) { return mod::IsFile(root / files::FromUtf8(m)); }))
                {
                    raw = root;
                    break;
                }
            }
            // the pause menu needs the Mod Tools sources; the map's own script menus are in its zones
            std::optional<std::string> pauseError;
            std::map<std::string, Json> menus;
            if (!raw)
                pauseError = "WaW menu sources (raw/ui/ingame.txt, raw/ui/hud.txt) not found; the WaW Mod Tools raw folder is required";
            else
            {
                // a raw/ui another mod edited may not parse: the map's script menus still stand
                try
                {
                    menus = LoadSources(*raw);
                }
                catch (const MenuSourceError& error)
                {
                    pauseError = error.what();
                    menus.clear();
                }
            }
            const auto glue = mod::Json(extract::ResourceRoot() / "src/waw2bo2/compat/waw_menu_glue.json");
            const auto& engine = glue.at("engine_menus");
            std::set<std::string> stop;
            for (const auto& [k, _] : engine.items())
                stop.insert(k);
            std::vector<std::string> names;
            if (!pauseError)
            {
                names = Closure(menus, stop, {ROOT_MENU});
                if (std::find(names.begin(), names.end(), ROOT_MENU) == names.end())
                    pauseError = ROOT_MENU + " is not defined by ui/ingame.txt, ui/hud.txt";
            }
            if (pauseError)
            {
                names.clear();
                if (scriptMenus.empty())
                    throw MenuSourceError(*pauseError);
            }
            // a map's menu of the same name replaces WaW's (it is loaded over common.ff)
            std::vector<std::string> scripted;
            for (const auto& [name, menu] : scriptMenus)
            {
                menus[name] = menu;
                scripted.push_back(name);
            }
            const auto stage = [&](std::vector<std::string> names) {
                for (const auto& name : Closure(menus, stop, scripted))
                    if (std::find(names.begin(), names.end(), name) == names.end())
                        names.push_back(name);
                const auto required = Requirements(menus, names, mapname);
                std::map<std::string, std::string> context{{"mapname", mapname}, {"scriptmenus", mod::Join(scripted, " ")}};
                for (const auto& [k, v] : engine.items())
                    context["engine:" + k] = v.at("t6_menu").get<std::string>();
                const auto table = CsvText(Encode(menus, names, context), "\n");
                files::WriteText(project / "zm/waw_menus.csv", table);
                Json result{{"source", raw ? mod::Str(*raw) : std::string()}, {"menus", names}, {"mapname", mapname}, {"script_menus", scripted}};
                for (const auto& [k, v] : required.items())
                    result[k] = v;
                result["engine_menus"] = std::vector<std::string>(stop.begin(), stop.end());
                files::WriteText(project / "content_source/waw_menu.json", util::JsonDumps(result, 2) + "\n");
                return result;
            };
            if (names.empty() || scripted.empty())
                return {stage(names), pauseError};
            // a pause menu another mod edited may parse and still not translate (UGX's objective_info.menu):
            // the map's own script menus stand without it
            try
            {
                return {stage(names), pauseError};
            }
            catch (const MenuSourceError& error)
            {
                return {stage({}), std::string(error.what())};
            }
        }
        catch (const MenuSourceError& error)
        {
            return {Json(), error.what()};
        }
    }
} // namespace w2b::pipeline

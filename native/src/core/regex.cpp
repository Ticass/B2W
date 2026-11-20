// A backtracking regex VM with Python `re` semantics (see regex.h): the
// pattern parses to a tree, compiles to instructions, and runs on code
// points with an explicit backtracking stack (no recursion limit).
#include "regex.h"

#include "encoding.h"
#include "unicode.h"

#include <windows.h>

#include <algorithm>
#include <map>

namespace w2b::regex
{
    namespace
    {
        char32_t Fold(const char32_t c)
        {
            if (c < 0x80)
                return (c >= 'A' && c <= 'Z') ? c + 32 : c;
            if (c > 0xFFFF)
                return c;
            wchar_t w = static_cast<wchar_t>(c);
            CharLowerBuffW(&w, 1);
            return w;
        }

        // ---- pattern tree ----------------------------------------------
        enum class N
        {
            Empty,
            Char,
            Any,
            Class,
            Concat,
            Alt,
            Repeat,
            Group,
            Bol,
            Eol,
            StrStart,
            StrEnd,
            WordB,
            NotWordB,
            Look,
            Backref
        };

        struct ClassItem
        {
            char32_t lo, hi; // range; category when lo == 0 && hi > 0x10FFFF
            int category;    // 0 none, 'd','D','w','W','s','S'
        };

        struct Node
        {
            N type = N::Empty;
            char32_t c = 0;
            bool icase = false, dotall = false, multiline = false;
            std::vector<ClassItem> items;
            bool negated = false;
            std::vector<std::unique_ptr<Node>> kids;
            int min = 0, max = -1;
            bool greedy = true;
            int group = -1;           // capture index (Group), -1 non-capturing
            bool lookNeg = false, lookBehind = false;
            int backref = 0;
        };

        bool InCategory(const int category, const char32_t c)
        {
            switch (category)
            {
            case 'd':
                return unicode::IsDigit(c);
            case 'D':
                return !unicode::IsDigit(c);
            case 'w':
                return unicode::IsWord(c);
            case 'W':
                return !unicode::IsWord(c);
            case 's':
                return unicode::IsSpace(c);
            case 'S':
                return !unicode::IsSpace(c);
            default:
                return false;
            }
        }

        bool ClassMatches(const Node& node, char32_t c)
        {
            const auto test = [&](const char32_t ch)
            {
                for (const auto& item : node.items)
                {
                    if (item.category)
                    {
                        if (InCategory(item.category, ch))
                            return true;
                    }
                    else if (ch >= item.lo && ch <= item.hi)
                        return true;
                }
                return false;
            };
            bool hit = test(c);
            if (!hit && node.icase)
            {
                const auto lower = Fold(c);
                hit = test(lower);
                if (!hit)
                {
                    // the upper-case form of c (ranges written in upper case)
                    wchar_t w = c <= 0xFFFF ? static_cast<wchar_t>(c) : 0;
                    if (w)
                    {
                        CharUpperBuffW(&w, 1);
                        hit = test(w);
                    }
                }
            }
            return hit != node.negated;
        }

        struct Parser
        {
            std::u32string p;
            size_t i = 0;
            unsigned flags;
            int groups = 0;
            std::map<std::string, int> names;

            explicit Parser(std::u32string pattern, const unsigned f)
                : p(std::move(pattern)),
                  flags(f)
            {
            }

            [[noreturn]] void Fail(const std::string& what) const
            {
                throw Error(what + " at position " + std::to_string(i));
            }

            bool More() const
            {
                return i < p.size();
            }

            void SkipVerbose(const unsigned f)
            {
                if (!(f & VERBOSE))
                    return;
                while (i < p.size())
                {
                    if (unicode::IsSpace(p[i]))
                        i++;
                    else if (p[i] == '#')
                    {
                        while (i < p.size() && p[i] != '\n')
                            i++;
                    }
                    else
                        break;
                }
            }

            // global inline flags at the very start: (?ims...)
            void LeadingFlags()
            {
                while (p.compare(i, 2, U"(?") == 0)
                {
                    size_t j = i + 2;
                    unsigned add = 0;
                    while (j < p.size() && std::u32string_view(U"aiLmsux").find(p[j]) != std::u32string_view::npos)
                    {
                        add |= p[j] == 'i' ? IGNORECASE : p[j] == 'm' ? MULTILINE : p[j] == 's' ? DOTALL : p[j] == 'x' ? VERBOSE : 0;
                        j++;
                    }
                    if (j < p.size() && p[j] == ')' && j > i + 2)
                    {
                        flags |= add;
                        i = j + 1;
                    }
                    else
                        break;
                }
            }

            std::unique_ptr<Node> ParseAlt(const unsigned f)
            {
                auto first = ParseConcat(f);
                if (!More() || p[i] != '|')
                    return first;
                auto alt = std::make_unique<Node>();
                alt->type = N::Alt;
                alt->kids.push_back(std::move(first));
                while (More() && p[i] == '|')
                {
                    i++;
                    alt->kids.push_back(ParseConcat(f));
                }
                return alt;
            }

            std::unique_ptr<Node> ParseConcat(const unsigned f)
            {
                auto seq = std::make_unique<Node>();
                seq->type = N::Concat;
                while (true)
                {
                    SkipVerbose(f);
                    if (!More() || p[i] == '|' || p[i] == ')')
                        break;
                    auto atom = ParseAtom(f);
                    if (!atom)
                        continue;
                    SkipVerbose(f);
                    atom = ParseQuantifier(std::move(atom), f);
                    seq->kids.push_back(std::move(atom));
                }
                return seq;
            }

            std::unique_ptr<Node> ParseQuantifier(std::unique_ptr<Node> atom, const unsigned f)
            {
                while (More())
                {
                    int min = -1, max = -1;
                    const size_t save = i;
                    if (p[i] == '*')
                    {
                        min = 0;
                        i++;
                    }
                    else if (p[i] == '+')
                    {
                        min = 1;
                        i++;
                    }
                    else if (p[i] == '?')
                    {
                        min = 0;
                        max = 1;
                        i++;
                    }
                    else if (p[i] == '{')
                    {
                        size_t j = i + 1;
                        std::string a, b;
                        while (j < p.size() && p[j] >= '0' && p[j] <= '9')
                            a += static_cast<char>(p[j++]);
                        bool comma = false;
                        if (j < p.size() && p[j] == ',')
                        {
                            comma = true;
                            j++;
                            while (j < p.size() && p[j] >= '0' && p[j] <= '9')
                                b += static_cast<char>(p[j++]);
                        }
                        if (j < p.size() && p[j] == '}' && (!a.empty() || comma))
                        {
                            min = a.empty() ? 0 : std::stoi(a);
                            max = comma ? (b.empty() ? -1 : std::stoi(b)) : min;
                            i = j + 1;
                        }
                        else
                            return atom; // a literal '{'
                    }
                    else
                        return atom;
                    (void)save;
                    auto rep = std::make_unique<Node>();
                    rep->type = N::Repeat;
                    rep->min = min;
                    rep->max = max;
                    if (More() && p[i] == '?')
                    {
                        rep->greedy = false;
                        i++;
                    }
                    rep->kids.push_back(std::move(atom));
                    atom = std::move(rep);
                    SkipVerbose(f);
                }
                return atom;
            }

            char32_t ParseEscapeChar(char32_t e)
            {
                switch (e)
                {
                case 'n':
                    return '\n';
                case 't':
                    return '\t';
                case 'r':
                    return '\r';
                case 'f':
                    return '\f';
                case 'v':
                    return '\v';
                case 'a':
                    return '\a';
                case 'x':
                {
                    const auto hex = p.substr(i, 2);
                    i += 2;
                    return static_cast<char32_t>(std::stoul(unicode::Encode(hex), nullptr, 16));
                }
                case 'u':
                {
                    const auto hex = p.substr(i, 4);
                    i += 4;
                    return static_cast<char32_t>(std::stoul(unicode::Encode(hex), nullptr, 16));
                }
                case 'U':
                {
                    const auto hex = p.substr(i, 8);
                    i += 8;
                    return static_cast<char32_t>(std::stoul(unicode::Encode(hex), nullptr, 16));
                }
                default:
                    return e;
                }
            }

            std::unique_ptr<Node> ParseClass(const unsigned f)
            {
                auto node = std::make_unique<Node>();
                node->type = N::Class;
                node->icase = f & IGNORECASE;
                if (More() && p[i] == '^')
                {
                    node->negated = true;
                    i++;
                }
                bool first = true;
                while (More() && (p[i] != ']' || first))
                {
                    first = false;
                    char32_t lo;
                    if (p[i] == '\\')
                    {
                        i++;
                        const char32_t e = p[i++];
                        if (std::u32string_view(U"dDwWsS").find(e) != std::u32string_view::npos)
                        {
                            node->items.push_back({0, 0, static_cast<int>(e)});
                            continue;
                        }
                        lo = e == 'b' ? U'\b' : ParseEscapeChar(e);
                    }
                    else
                        lo = p[i++];
                    char32_t hi = lo;
                    if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']')
                    {
                        i++;
                        if (p[i] == '\\')
                        {
                            i++;
                            hi = ParseEscapeChar(p[i++]);
                        }
                        else
                            hi = p[i++];
                    }
                    node->items.push_back({lo, hi, 0});
                }
                if (!More())
                    Fail("unterminated character set");
                i++; // ']'
                return node;
            }

            std::unique_ptr<Node> ParseAtom(unsigned f)
            {
                const char32_t c = p[i];
                auto node = std::make_unique<Node>();
                node->icase = f & IGNORECASE;
                node->dotall = f & DOTALL;
                node->multiline = f & MULTILINE;
                if (c == '(')
                {
                    i++;
                    if (p.compare(i, 1, U"?") == 0)
                    {
                        i++;
                        const char32_t k = p[i];
                        if (k == ':')
                        {
                            i++;
                            node->type = N::Group;
                            node->kids.push_back(ParseAlt(f));
                        }
                        else if (k == '#')
                        {
                            while (More() && p[i] != ')')
                                i++;
                            i++;
                            return nullptr;
                        }
                        else if (k == '=' || k == '!')
                        {
                            i++;
                            node->type = N::Look;
                            node->lookNeg = k == '!';
                            node->kids.push_back(ParseAlt(f));
                        }
                        else if (k == '<' && i + 1 < p.size() && (p[i + 1] == '=' || p[i + 1] == '!'))
                        {
                            node->type = N::Look;
                            node->lookBehind = true;
                            node->lookNeg = p[i + 1] == '!';
                            i += 2;
                            node->kids.push_back(ParseAlt(f));
                        }
                        else if (k == 'P' && i + 1 < p.size() && p[i + 1] == '<')
                        {
                            i += 2;
                            std::u32string name;
                            while (More() && p[i] != '>')
                                name += p[i++];
                            i++;
                            node->type = N::Group;
                            node->group = ++groups;
                            names[unicode::Encode(name)] = node->group;
                            node->kids.push_back(ParseAlt(f));
                        }
                        else if (k == 'P' && i + 1 < p.size() && p[i + 1] == '=')
                        {
                            i += 2;
                            std::u32string name;
                            while (More() && p[i] != ')')
                                name += p[i++];
                            i++;
                            node->type = N::Backref;
                            node->backref = names.at(unicode::Encode(name));
                            return node;
                        }
                        else
                        {
                            // scoped flags (?imsx-imsx:...)
                            unsigned on = 0, off = 0;
                            bool negative = false;
                            while (More() && p[i] != ':' && p[i] != ')')
                            {
                                const char32_t flag = p[i++];
                                if (flag == '-')
                                {
                                    negative = true;
                                    continue;
                                }
                                const unsigned bit = flag == 'i' ? IGNORECASE : flag == 'm' ? MULTILINE : flag == 's' ? DOTALL : flag == 'x' ? VERBOSE : 0;
                                (negative ? off : on) |= bit;
                            }
                            if (!More() || p[i] != ':')
                                Fail("global flags not at the start of the expression");
                            i++;
                            node->type = N::Group;
                            node->kids.push_back(ParseAlt((f | on) & ~off));
                        }
                    }
                    else
                    {
                        node->type = N::Group;
                        node->group = ++groups;
                        node->kids.push_back(ParseAlt(f));
                    }
                    if (!More() || p[i] != ')')
                        Fail("missing ), unterminated subpattern");
                    i++;
                    return node;
                }
                i++;
                switch (c)
                {
                case '.':
                    node->type = N::Any;
                    return node;
                case '^':
                    node->type = N::Bol;
                    return node;
                case '$':
                    node->type = N::Eol;
                    return node;
                case '[':
                    return ParseClass(f);
                case '\\':
                {
                    const char32_t e = p[i++];
                    if (e == 'A')
                        node->type = N::StrStart;
                    else if (e == 'Z')
                        node->type = N::StrEnd;
                    else if (e == 'b')
                        node->type = N::WordB;
                    else if (e == 'B')
                        node->type = N::NotWordB;
                    else if (std::u32string_view(U"dDwWsS").find(e) != std::u32string_view::npos)
                    {
                        node->type = N::Class;
                        node->items.push_back({0, 0, static_cast<int>(e)});
                    }
                    else if (e >= '1' && e <= '9')
                    {
                        int n = static_cast<int>(e - '0');
                        if (More() && p[i] >= '0' && p[i] <= '9')
                            n = n * 10 + static_cast<int>(p[i++] - '0');
                        node->type = N::Backref;
                        node->backref = n;
                    }
                    else
                    {
                        node->type = N::Char;
                        node->c = ParseEscapeChar(e);
                    }
                    return node;
                }
                default:
                    node->type = N::Char;
                    node->c = c;
                    return node;
                }
            }
        };

        // ---- instructions ----------------------------------------------
        enum class Op
        {
            Char,
            Any,
            Class,
            Split, // try x, then y
            Jmp,
            Save,
            Match,
            Bol,
            Eol,
            StrStart,
            StrEnd,
            WordB,
            NotWordB,
            Look,
            Backref,
            Mark,    // mark[reg] = pos (iteration start)
            Progress // pos == mark[reg]: the iteration was empty, leave the loop (to x)
        };

        struct Inst
        {
            Op op;
            int x = 0, y = 0;
            const Node* node = nullptr;
            int sub = -1;       // Look: sub-program index
            int width = 0;      // Look behind: fixed width
        };

        struct SubProgram
        {
            std::vector<Inst> code;
        };
    } // namespace

    struct Program
    {
        std::u32string pattern;
        std::vector<std::unique_ptr<Node>> trees;
        std::vector<SubProgram> subs; // 0: the pattern
        int groups = 0;
        int marks = 0;
        std::map<std::string, int> names;
    };

    namespace
    {
        int Width(const Node& n)
        {
            switch (n.type)
            {
            case N::Char:
            case N::Any:
            case N::Class:
                return 1;
            case N::Concat:
            {
                int w = 0;
                for (const auto& k : n.kids)
                {
                    const int kw = Width(*k);
                    if (kw < 0)
                        return -1;
                    w += kw;
                }
                return w;
            }
            case N::Alt:
            {
                int w = -2;
                for (const auto& k : n.kids)
                {
                    const int kw = Width(*k);
                    if (kw < 0 || (w != -2 && kw != w))
                        return -1;
                    w = kw;
                }
                return w < 0 ? 0 : w;
            }
            case N::Group:
                return Width(*n.kids[0]);
            case N::Repeat:
            {
                if (n.max != n.min)
                    return -1;
                const int kw = Width(*n.kids[0]);
                return kw < 0 ? -1 : kw * n.min;
            }
            case N::Backref:
                return -1;
            default:
                return 0;
            }
        }

        struct Compiler
        {
            Program& program;

            void Emit(std::vector<Inst>& code, const Node& n)
            {
                switch (n.type)
                {
                case N::Empty:
                    return;
                case N::Char:
                    code.push_back({Op::Char, 0, 0, &n});
                    return;
                case N::Any:
                    code.push_back({Op::Any, 0, 0, &n});
                    return;
                case N::Class:
                    code.push_back({Op::Class, 0, 0, &n});
                    return;
                case N::Bol:
                    code.push_back({Op::Bol, 0, 0, &n});
                    return;
                case N::Eol:
                    code.push_back({Op::Eol, 0, 0, &n});
                    return;
                case N::StrStart:
                    code.push_back({Op::StrStart});
                    return;
                case N::StrEnd:
                    code.push_back({Op::StrEnd});
                    return;
                case N::WordB:
                    code.push_back({Op::WordB});
                    return;
                case N::NotWordB:
                    code.push_back({Op::NotWordB});
                    return;
                case N::Backref:
                    code.push_back({Op::Backref, n.backref, 0, &n});
                    return;
                case N::Concat:
                    for (const auto& k : n.kids)
                        Emit(code, *k);
                    return;
                case N::Group:
                    if (n.group >= 0)
                        code.push_back({Op::Save, 2 * n.group});
                    Emit(code, *n.kids[0]);
                    if (n.group >= 0)
                        code.push_back({Op::Save, 2 * n.group + 1});
                    return;
                case N::Alt:
                {
                    std::vector<size_t> jumps;
                    for (size_t k = 0; k < n.kids.size(); k++)
                    {
                        if (k + 1 < n.kids.size())
                        {
                            const size_t split = code.size();
                            code.push_back({Op::Split});
                            code[split].x = static_cast<int>(code.size());
                            Emit(code, *n.kids[k]);
                            jumps.push_back(code.size());
                            code.push_back({Op::Jmp});
                            code[split].y = static_cast<int>(code.size());
                        }
                        else
                            Emit(code, *n.kids[k]);
                    }
                    for (const auto j : jumps)
                        code[j].x = static_cast<int>(code.size());
                    return;
                }
                case N::Look:
                {
                    SubProgram sub;
                    Emit(sub.code, *n.kids[0]);
                    sub.code.push_back({Op::Match});
                    int width = 0;
                    if (n.lookBehind)
                    {
                        width = Width(*n.kids[0]);
                        if (width < 0)
                            throw Error("look-behind requires fixed-width pattern");
                    }
                    program.subs.push_back(std::move(sub));
                    code.push_back({Op::Look, 0, 0, &n, static_cast<int>(program.subs.size() - 1), width});
                    return;
                }
                case N::Repeat:
                {
                    const Node& body = *n.kids[0];
                    for (int k = 0; k < n.min; k++)
                        Emit(code, body);
                    if (n.max == n.min)
                        return;
                    if (n.max < 0)
                    {
                        // L: split(body, exit) / mark / body / progress(exit) / jmp L
                        const int reg = program.marks++;
                        const size_t loop = code.size();
                        code.push_back({Op::Split});
                        const size_t bodyStart = code.size();
                        code.push_back({Op::Mark, reg});
                        Emit(code, body);
                        const size_t progress = code.size();
                        code.push_back({Op::Progress, 0, reg});
                        code.push_back({Op::Jmp, static_cast<int>(loop)});
                        const int exit = static_cast<int>(code.size());
                        code[loop].x = n.greedy ? static_cast<int>(bodyStart) : exit;
                        code[loop].y = n.greedy ? exit : static_cast<int>(bodyStart);
                        code[progress].x = exit;
                        return;
                    }
                    std::vector<size_t> splits;
                    for (int k = n.min; k < n.max; k++)
                    {
                        splits.push_back(code.size());
                        code.push_back({Op::Split});
                        code.back().x = static_cast<int>(code.size()); // provisional: body
                        Emit(code, body);
                    }
                    const int exit = static_cast<int>(code.size());
                    for (const auto s : splits)
                    {
                        const int bodyStart = static_cast<int>(s + 1);
                        code[s].x = n.greedy ? bodyStart : exit;
                        code[s].y = n.greedy ? exit : bodyStart;
                    }
                    return;
                }
                }
            }
        };

        struct Thread
        {
            int kind; // 0 branch (pc,pos), 1 restore save slot, 2 restore mark
            int pc;
            long long pos;
            int slot;
            long long old;
        };

        // requireEnd: fullmatch; forbidEmpty: the match must not be empty
        // (after an empty match): both are checked where Python's sre checks
        // them, at success, so a failing path backtracks into the alternatives
        bool Run(const Program& program, const int subIndex, const std::u32string& s, const long long start, std::vector<long long>& caps,
                 std::vector<long long>& marks, long long* endOut, const bool requireEnd = false, const bool forbidEmpty = false)
        {
            const auto& code = program.subs[static_cast<size_t>(subIndex)].code;
            const long long n = static_cast<long long>(s.size());
            std::vector<Thread> stack;
            int pc = 0;
            long long pos = start;
            const auto fail = [&]() -> bool
            {
                while (!stack.empty())
                {
                    const Thread t = stack.back();
                    stack.pop_back();
                    if (t.kind == 1)
                        caps[static_cast<size_t>(t.slot)] = t.old;
                    else if (t.kind == 2)
                        marks[static_cast<size_t>(t.slot)] = t.old;
                    else
                    {
                        pc = t.pc;
                        pos = t.pos;
                        return true;
                    }
                }
                return false;
            };
            while (true)
            {
                const Inst& in = code[static_cast<size_t>(pc)];
                bool ok = true;
                switch (in.op)
                {
                case Op::Match:
                    if ((requireEnd && pos != n) || (forbidEmpty && pos == start))
                    {
                        ok = false;
                        break;
                    }
                    *endOut = pos;
                    return true;
                case Op::Char:
                    ok = pos < n && (in.node->icase ? Fold(s[static_cast<size_t>(pos)]) == Fold(in.node->c) : s[static_cast<size_t>(pos)] == in.node->c);
                    if (ok)
                    {
                        pos++;
                        pc++;
                    }
                    break;
                case Op::Any:
                    ok = pos < n && (in.node->dotall || s[static_cast<size_t>(pos)] != '\n');
                    if (ok)
                    {
                        pos++;
                        pc++;
                    }
                    break;
                case Op::Class:
                    ok = pos < n && ClassMatches(*in.node, s[static_cast<size_t>(pos)]);
                    if (ok)
                    {
                        pos++;
                        pc++;
                    }
                    break;
                case Op::Split:
                    stack.push_back({0, in.y, pos, 0, 0});
                    pc = in.x;
                    break;
                case Op::Jmp:
                    pc = in.x;
                    break;
                case Op::Save:
                    stack.push_back({1, 0, 0, in.x, caps[static_cast<size_t>(in.x)]});
                    caps[static_cast<size_t>(in.x)] = pos;
                    pc++;
                    break;
                case Op::Mark:
                    stack.push_back({2, 0, 0, in.x, marks[static_cast<size_t>(in.x)]});
                    marks[static_cast<size_t>(in.x)] = pos;
                    pc++;
                    break;
                case Op::Progress:
                    pc = pos == marks[static_cast<size_t>(in.y)] ? in.x : pc + 1;
                    break;
                case Op::Bol:
                    ok = pos == 0 || (in.node->multiline && s[static_cast<size_t>(pos - 1)] == '\n');
                    pc++;
                    break;
                case Op::Eol:
                    ok = pos == n || (pos == n - 1 && s[static_cast<size_t>(pos)] == '\n') || (in.node->multiline && s[static_cast<size_t>(pos)] == '\n');
                    pc++;
                    break;
                case Op::StrStart:
                    ok = pos == 0;
                    pc++;
                    break;
                case Op::StrEnd:
                    ok = pos == n;
                    pc++;
                    break;
                case Op::WordB:
                case Op::NotWordB:
                {
                    const bool before = pos > 0 && unicode::IsWord(s[static_cast<size_t>(pos - 1)]);
                    const bool after = pos < n && unicode::IsWord(s[static_cast<size_t>(pos)]);
                    ok = (before != after) == (in.op == Op::WordB);
                    pc++;
                    break;
                }
                case Op::Backref:
                {
                    const auto a = caps[static_cast<size_t>(2 * in.x)], b = caps[static_cast<size_t>(2 * in.x + 1)];
                    if (a < 0 || b < 0)
                    {
                        ok = false;
                        break;
                    }
                    const long long len = b - a;
                    ok = pos + len <= n;
                    for (long long k = 0; ok && k < len; k++)
                    {
                        const auto x = s[static_cast<size_t>(a + k)], y = s[static_cast<size_t>(pos + k)];
                        ok = in.node->icase ? Fold(x) == Fold(y) : x == y;
                    }
                    if (ok)
                    {
                        pos += len;
                        pc++;
                    }
                    break;
                }
                case Op::Look:
                {
                    const long long from = in.node->lookBehind ? pos - in.width : pos;
                    bool found = false;
                    std::vector<long long> inner = caps;
                    if (from >= 0)
                    {
                        long long end = -1;
                        found = Run(program, in.sub, s, from, inner, marks, &end) && (!in.node->lookBehind || end == pos);
                    }
                    ok = found != in.node->lookNeg;
                    if (ok && found && !in.node->lookNeg)
                    {
                        for (size_t k = 0; k < caps.size(); k++)
                            if (inner[k] != caps[k])
                            {
                                stack.push_back({1, 0, 0, static_cast<int>(k), caps[k]});
                                caps[k] = inner[k];
                            }
                    }
                    pc++;
                    break;
                }
                }
                if (!ok && !fail())
                    return false;
            }
        }

        std::shared_ptr<const std::u32string> Text(const std::string_view utf8)
        {
            return std::make_shared<const std::u32string>(unicode::Decode(utf8));
        }
    } // namespace

    // ---- Match ---------------------------------------------------------
    std::optional<std::string> Match::Group(const size_t n) const
    {
        const auto a = spans.at(2 * n), b = spans.at(2 * n + 1);
        if (a < 0 || b < 0)
            return std::nullopt;
        return unicode::Encode(std::u32string_view(*text).substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
    }

    std::optional<std::string> Match::Group(const std::string& name) const
    {
        return Group(static_cast<size_t>(program->names.at(name)));
    }

    std::string Match::operator[](const size_t n) const
    {
        return Group(n).value_or("");
    }

    long long Match::Start(const size_t n) const
    {
        return spans.at(2 * n);
    }

    long long Match::End(const size_t n) const
    {
        return spans.at(2 * n + 1);
    }

    size_t Match::Groups() const
    {
        return static_cast<size_t>(program->groups);
    }

    // ---- Regex ---------------------------------------------------------
    Regex::Regex(const std::string_view pattern, const unsigned flags)
    {
        auto program = std::make_shared<Program>();
        program->pattern = unicode::Decode(pattern);
        Parser parser(program->pattern, flags);
        parser.LeadingFlags();
        auto tree = parser.ParseAlt(parser.flags);
        if (parser.More())
            parser.Fail(parser.p[parser.i] == ')' ? "unbalanced parenthesis" : "unexpected end of pattern");
        program->groups = parser.groups;
        program->names = parser.names;
        Compiler compiler{*program};
        SubProgram main;
        main.code.push_back({Op::Save, 0});
        compiler.Emit(main.code, *tree);
        main.code.push_back({Op::Save, 1});
        main.code.push_back({Op::Match});
        program->subs.insert(program->subs.begin(), std::move(main));
        // look sub-program indices shifted by the insert
        for (size_t k = 0; k < program->subs.size(); k++)
            for (auto& inst : program->subs[k].code)
                if (inst.op == Op::Look)
                    inst.sub += 1;
        program->trees.push_back(std::move(tree));
        m_program = std::move(program);
    }

    size_t Regex::Groups() const
    {
        return static_cast<size_t>(m_program->groups);
    }

    // mode 0: search from pos, 1: match at pos, 2: fullmatch
    std::optional<Match> Regex::Run(const std::shared_ptr<const std::u32string>& text, const size_t pos, const int mode, const bool mustAdvance,
                                    const size_t from) const
    {
        const auto& s = *text;
        const size_t last = mode == 0 ? s.size() : pos;
        for (size_t start = pos; start <= last; start++)
        {
            std::vector<long long> caps(static_cast<size_t>(2 * (m_program->groups + 1)), -1);
            std::vector<long long> marks(static_cast<size_t>(m_program->marks), -1);
            long long end = -1;
            if (!w2b::regex::Run(*m_program, 0, s, static_cast<long long>(start), caps, marks, &end, mode == 2, mustAdvance && start == from))
                continue;
            Match m;
            m.program = m_program;
            m.text = text;
            m.spans = std::move(caps);
            return m;
        }
        return std::nullopt;
    }

    std::optional<Match> Regex::Search(const std::string_view text, const size_t pos) const
    {
        return Run(Text(text), pos, 0, false, 0);
    }

    std::optional<Match> Regex::MatchAt(const std::string_view text, const size_t pos) const
    {
        return Run(Text(text), pos, 1, false, 0);
    }

    std::optional<Match> Regex::FullMatch(const std::string_view text) const
    {
        return Run(Text(text), 0, 2, false, 0);
    }

    std::vector<Match> Regex::FindIter(const std::string_view utf8) const
    {
        const auto text = Text(utf8);
        std::vector<Match> out;
        size_t pos = 0;
        bool mustAdvance = false;
        while (pos <= text->size())
        {
            auto m = Run(text, pos, 0, mustAdvance, pos);
            if (!m)
                break;
            const auto b = static_cast<size_t>(m->Start()), e = static_cast<size_t>(m->End());
            mustAdvance = b == e;
            pos = e;
            out.push_back(std::move(*m));
            (void)b;
        }
        return out;
    }

    std::vector<std::vector<std::string>> Regex::FindAll(const std::string_view text) const
    {
        std::vector<std::vector<std::string>> out;
        for (const auto& m : FindIter(text))
        {
            std::vector<std::string> row;
            if (m_program->groups == 0)
                row.push_back(m[0]);
            else
                for (int g = 1; g <= m_program->groups; g++)
                    row.push_back(m[static_cast<size_t>(g)]);
            out.push_back(std::move(row));
        }
        return out;
    }

    namespace
    {
        // re.sub template expansion
        std::string Expand(const std::string_view tmpl, const Match& m)
        {
            const auto t = unicode::Decode(tmpl);
            std::string out;
            for (size_t i = 0; i < t.size(); i++)
            {
                if (t[i] != '\\' || i + 1 >= t.size())
                {
                    encoding::AppendUtf8(out, static_cast<unsigned>(t[i]));
                    continue;
                }
                const char32_t e = t[++i];
                if (e == 'g' && i + 1 < t.size() && t[i + 1] == '<')
                {
                    const auto close = t.find(U'>', i);
                    const auto name = unicode::Encode(t.substr(i + 2, close - i - 2));
                    i = close;
                    if (!name.empty() && std::all_of(name.begin(), name.end(), [](const char c) { return c >= '0' && c <= '9'; }))
                        out += m[static_cast<size_t>(std::stoi(name))];
                    else
                        out += m.Group(name).value_or("");
                }
                else if (e >= '0' && e <= '9')
                {
                    int n = static_cast<int>(e - '0');
                    if (i + 1 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9')
                        n = n * 10 + static_cast<int>(t[++i] - '0');
                    out += m[static_cast<size_t>(n)];
                }
                else if (e == 'n')
                    out += '\n';
                else if (e == 't')
                    out += '\t';
                else if (e == 'r')
                    out += '\r';
                else if (e == 'f')
                    out += '\f';
                else if (e == 'v')
                    out += '\v';
                else if (e == 'a')
                    out += '\a';
                else if (e == 'b')
                    out += '\b';
                else if (e == '\\')
                    out += '\\';
                else
                {
                    out += '\\';
                    encoding::AppendUtf8(out, static_cast<unsigned>(e));
                }
            }
            return out;
        }
    } // namespace

    std::string Regex::Sub(const std::function<std::string(const Match&)>& replace, const std::string_view utf8, const size_t count, size_t* made) const
    {
        const auto text = Text(utf8);
        std::string out;
        size_t pos = 0, last = 0, n = 0;
        bool mustAdvance = false;
        while ((!count || n < count) && pos <= text->size())
        {
            auto m = Run(text, pos, 0, mustAdvance, pos);
            if (!m)
                break;
            const auto b = static_cast<size_t>(m->Start()), e = static_cast<size_t>(m->End());
            out += unicode::Encode(std::u32string_view(*text).substr(last, b - last));
            out += replace(*m);
            last = e;
            n++;
            mustAdvance = b == e;
            pos = e;
        }
        out += unicode::Encode(std::u32string_view(*text).substr(last));
        if (made)
            *made = n;
        return out;
    }

    std::string Regex::Sub(const std::string_view replacement, const std::string_view text, const size_t count, size_t* made) const
    {
        const std::string tmpl(replacement);
        return Sub([&](const Match& m) { return Expand(tmpl, m); }, text, count, made);
    }

    std::vector<std::string> Regex::Split(const std::string_view utf8, const size_t maxsplit) const
    {
        const auto text = Text(utf8);
        std::vector<std::string> out;
        size_t pos = 0, last = 0, n = 0;
        bool mustAdvance = false;
        while ((!maxsplit || n < maxsplit) && pos <= text->size())
        {
            auto m = Run(text, pos, 0, mustAdvance, pos);
            if (!m)
                break;
            const auto b = static_cast<size_t>(m->Start()), e = static_cast<size_t>(m->End());
            out.push_back(unicode::Encode(std::u32string_view(*text).substr(last, b - last)));
            for (int g = 1; g <= m_program->groups; g++)
                out.push_back((*m)[static_cast<size_t>(g)]);
            last = e;
            n++;
            mustAdvance = b == e;
            pos = e;
        }
        out.push_back(unicode::Encode(std::u32string_view(*text).substr(last)));
        return out;
    }

    std::string Escape(const std::string_view text)
    {
        std::string out;
        for (const char c : text)
        {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x80 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                out += '\\';
            out += c;
        }
        return out;
    }
} // namespace w2b::regex

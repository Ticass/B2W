#include "ast.h"

#include <array>
#include <optional>
#include <string_view>

namespace w2b::gsc
{
    namespace
    {
        // A token as T4 reads it: "[[" and "]]" are two brackets.
        struct Lexeme
        {
            uint32_t token = 0;
            std::string_view text;
            Kind kind = Kind::Eof;
        };

        std::vector<Lexeme> Lexemes(const std::vector<Token>& tokens)
        {
            std::vector<Lexeme> out;
            out.reserve(tokens.size() + 16);
            for (uint32_t i = 0; i < tokens.size(); i++)
            {
                const auto& t = tokens[i];
                const std::string_view text = t.text;
                if (t.kind == Kind::Punct && (text == "[[" || text == "]]"))
                {
                    out.push_back({i, text.substr(0, 1), Kind::Punct});
                    out.push_back({i, text.substr(1, 1), Kind::Punct});
                }
                else
                    out.push_back({i, text, t.kind});
            }
            return out;
        }

        // T4's keywords are lower case only
        bool IsKeyword(const std::string_view text)
        {
            static constexpr std::array<std::string_view, 28> keywords{
                "return",   "wait",          "thread",           "undefined", "self",   "level", "game",    "anim",  "if",
                "else",     "while",         "for",              "waittill",  "notify", "endon", "switch",  "case",  "default",
                "break",    "continue",      "waittillmatch",    "false",     "true",   "breakpoint",       "prof_begin",
                "prof_end", "breakon",       "waittillframeend"};
            for (const auto word : keywords)
                if (word == text)
                    return true;
            return false;
        }

        // all left associative; "!" and "~" bind tighter than any
        int Precedence(const std::string_view op)
        {
            static constexpr std::array<std::pair<std::string_view, int>, 18> table{{{"||", 1},
                                                                                     {"&&", 2},
                                                                                     {"|", 3},
                                                                                     {"^", 4},
                                                                                     {"&", 5},
                                                                                     {"==", 6},
                                                                                     {"!=", 6},
                                                                                     {"<", 7},
                                                                                     {">", 7},
                                                                                     {"<=", 7},
                                                                                     {">=", 7},
                                                                                     {"<<", 8},
                                                                                     {">>", 8},
                                                                                     {"+", 9},
                                                                                     {"-", 9},
                                                                                     {"*", 10},
                                                                                     {"/", 10},
                                                                                     {"%", 10}}};
            for (const auto& [text, precedence] : table)
                if (text == op)
                    return precedence;
            return 0;
        }

        class Parser
        {
        public:
            Parser(const std::vector<Token>& tokens, const Dialect dialect) : tokens_(tokens), lex_(Lexemes(tokens)), dialect_(dialect)
            {
                ast_.nodes.reserve(tokens.size());
            }

            Ast Run()
            {
                const auto root = Open(NodeKind::Script);
                while (!AtEnd())
                    Add(root, TopLevel());
                ast_.root = Close(root);
                return std::move(ast_);
            }

        private:
            // ---- lexemes
            const Lexeme& Peek(const size_t ahead = 0) const
            {
                const auto i = pos_ + ahead;
                return lex_[i < lex_.size() ? i : lex_.size() - 1];
            }

            bool AtEnd() const
            {
                return Peek().kind == Kind::Eof;
            }

            bool Is(const std::string_view text, const size_t ahead = 0) const
            {
                const auto& t = Peek(ahead);
                return (t.kind == Kind::Punct || t.kind == Kind::Ident) && t.text == text;
            }

            bool IsIdent(const size_t ahead = 0) const
            {
                const auto& t = Peek(ahead);
                return t.kind == Kind::Ident && !IsKeyword(t.text);
            }

            bool IsPath(const size_t ahead = 0) const
            {
                return Peek(ahead).kind == Kind::Path || IsIdent(ahead);
            }

            [[noreturn]] void Fail() const
            {
                const auto& t = Peek();
                throw GscSyntaxError("line " + std::to_string(tokens_[t.token].line) + ": "
                                     + (AtEnd() ? "unexpected end of file found" : "bad syntax at " + Repr(t.text)));
            }

            void Expect(const std::string_view text)
            {
                if (!Accept(text))
                    Fail();
            }

            bool Accept(const std::string_view text)
            {
                if (!Is(text))
                    return false;
                pos_++;
                return true;
            }

            // ---- nodes
            NodeId Open(const NodeKind kind)
            {
                Node node;
                node.kind = kind;
                node.token = node.begin = Peek().token;
                ast_.nodes.push_back(std::move(node));
                return static_cast<NodeId>(ast_.nodes.size() - 1);
            }

            // a node whose first kid is already parsed
            NodeId OpenAt(const NodeKind kind, const NodeId first)
            {
                const auto begin = ast_.nodes[first].begin;
                const auto id = Open(kind);
                ast_.nodes[id].begin = begin;
                return id;
            }

            NodeId Close(const NodeId id)
            {
                ast_.nodes[id].end = pos_ ? lex_[pos_ - 1].token + 1 : 0;
                return id;
            }

            void Add(const NodeId parent, const NodeId kid)
            {
                ast_.nodes[parent].kids.push_back(kid);
            }

            NodeId Leaf(const NodeKind kind, const size_t lexemes = 1)
            {
                const auto id = Open(kind);
                pos_ += lexemes;
                ast_.nodes[id].token = lex_[pos_ - 1].token;
                return Close(id);
            }

            NodeId Absent()
            {
                const auto id = Open(NodeKind::Empty);
                ast_.nodes[id].end = ast_.nodes[id].begin;
                return id;
            }

            // ---- top level
            NodeId TopLevel()
            {
                if (Peek().kind == Kind::Directive)
                {
                    const auto id = Leaf(NodeKind::Directive);
                    Accept(";"); // written on the next line
                    return id;
                }
                if (Is("/#"))
                    return Leaf(NodeKind::DevBegin);
                if (Is("#/"))
                    return Leaf(NodeKind::DevEnd);
                if (!IsIdent())
                    Fail();
                const auto id = Open(NodeKind::Function);
                pos_++;
                Expect("(");
                if (!Is(")"))
                    do
                    {
                        if (!IsIdent())
                            Fail();
                        Add(id, Leaf(NodeKind::Param));
                    } while (Accept(","));
                Expect(")");
                Add(id, Block());
                return Close(id);
            }

            // ---- statements
            NodeId Block()
            {
                const auto id = Open(NodeKind::Block);
                Expect("{");
                while (!Is("}"))
                    Add(id, Item());
                pos_++;
                return Close(id);
            }

            // what may stand in a block
            NodeId Item()
            {
                if (Is(";"))
                    return Leaf(NodeKind::Empty);
                if (Is("case"))
                {
                    const auto id = Open(NodeKind::Case);
                    pos_++;
                    Add(id, Primary());
                    Expect(":");
                    return Close(id);
                }
                if (Is("default"))
                {
                    const auto id = Open(NodeKind::Default);
                    pos_++;
                    Expect(":");
                    return Close(id);
                }
                return Statement();
            }

            NodeId Statement()
            {
                if (Is("{"))
                    return Block();
                if (Is("if"))
                {
                    const auto id = Open(NodeKind::If);
                    pos_++;
                    Add(id, Condition());
                    Add(id, Statement());
                    if (Accept("else"))
                        Add(id, Statement());
                    return Close(id);
                }
                if (Is("while"))
                {
                    const auto id = Open(NodeKind::While);
                    pos_++;
                    Add(id, Condition());
                    Add(id, Statement());
                    return Close(id);
                }
                if (Is("for"))
                    return For();
                if (dialect_ == Dialect::Bo2 && Is("foreach") && Is("(", 1))
                    return ForEach();
                if (dialect_ == Dialect::Bo2 && Is("do") && !Is("(", 1) && !Is("=", 1) && !Is(".", 1))
                {
                    const auto id = Open(NodeKind::DoWhile);
                    pos_++;
                    Add(id, Statement());
                    Expect("while");
                    Add(id, Condition());
                    Expect(";");
                    return Close(id);
                }
                if (Is("switch"))
                {
                    const auto id = Open(NodeKind::Switch);
                    pos_++;
                    Add(id, Condition());
                    Add(id, Block());
                    return Close(id);
                }
                if (Is("/#"))
                {
                    const auto id = Open(NodeKind::DevBlock);
                    pos_++;
                    while (!Is("#/"))
                        Add(id, Item());
                    pos_++;
                    return Close(id);
                }
                const auto id = Simple();
                Expect(";");
                return Close(id); // with its ";"
            }

            NodeId For()
            {
                const auto id = Open(NodeKind::For);
                pos_++;
                Expect("(");
                if (Is(";"))
                {
                    Add(id, Absent());
                    pos_++;
                }
                else
                    Add(id, Item()); // a statement with its ";"
                Add(id, Is(";") ? Absent() : Expression());
                Expect(";");
                Add(id, Is(")") ? Absent() : Simple());
                Expect(")");
                Add(id, Statement());
                return Close(id);
            }

            NodeId ForEach()
            {
                const auto id = Open(NodeKind::ForEach);
                pos_++;
                Expect("(");
                do
                {
                    if (!IsIdent())
                        Fail();
                    Add(id, Leaf(NodeKind::Local));
                } while (Accept(","));
                Expect("in");
                Add(id, Expression());
                Expect(")");
                Add(id, Statement());
                return Close(id);
            }

            NodeId Condition()
            {
                Expect("(");
                const auto id = Expression();
                Expect(")");
                return id;
            }

            // a statement without its ";"
            NodeId Simple()
            {
                if (Is("return"))
                {
                    const auto id = Open(NodeKind::Return);
                    pos_++;
                    if (!Is(";") && !Is(")"))
                        Add(id, Expression());
                    return Close(id);
                }
                if (Is("wait"))
                {
                    const auto id = Open(NodeKind::Wait);
                    pos_++;
                    Add(id, Expression());
                    return Close(id);
                }
                if (Is("waittillframeend"))
                    return Leaf(NodeKind::WaitTillFrameEnd);
                if (Is("break"))
                    return Leaf(NodeKind::Break);
                if (Is("continue"))
                    return Leaf(NodeKind::Continue);
                if (Is("breakpoint"))
                    return Leaf(NodeKind::Breakpoint);
                if (Is("prof_begin") || Is("prof_end"))
                {
                    const auto id = Open(NodeKind::Prof);
                    pos_++;
                    Expect("(");
                    if (Peek().kind != Kind::String)
                        Fail();
                    Add(id, Leaf(NodeKind::String));
                    Expect(")");
                    return Close(id);
                }
                const auto target = Primary();
                const auto kind = ast_.nodes[target].kind;
                if (Is("waittill"))
                    return Event(NodeKind::WaitTill, target);
                if (Is("waittillmatch"))
                    return Event(NodeKind::WaitTillMatch, target);
                if (Is("notify"))
                    return Event(NodeKind::Notify, target);
                if (Is("endon"))
                    return Event(NodeKind::EndOn, target);
                if (kind == NodeKind::Local || kind == NodeKind::Field || kind == NodeKind::Index)
                {
                    if (Is("++") || Is("--"))
                    {
                        const auto id = OpenAt(Is("++") ? NodeKind::Increment : NodeKind::Decrement, target);
                        Add(id, target);
                        pos_++;
                        return Close(id);
                    }
                    static constexpr std::array<std::string_view, 11> operators{"=", "|=", "^=", "&=", "<<=", ">>=", "+=", "-=", "*=", "/=", "%="};
                    for (const auto op : operators)
                        if (Is(op))
                        {
                            const auto id = OpenAt(NodeKind::Assign, target);
                            Add(id, target);
                            pos_++;
                            Add(id, Expression());
                            return Close(id);
                        }
                }
                if (kind != NodeKind::Call)
                    Fail();
                const auto id = OpenAt(NodeKind::ExprStmt, target);
                ast_.nodes[id].token = ast_.nodes[target].token;
                Add(id, target);
                return Close(id);
            }

            NodeId Event(const NodeKind kind, const NodeId object)
            {
                const auto id = OpenAt(kind, object);
                Add(id, object);
                pos_++;
                Expect("(");
                Add(id, Expression());
                while (kind != NodeKind::EndOn && Accept(","))
                {
                    if (kind == NodeKind::WaitTill && !IsIdent() && !(dialect_ == Dialect::Bo2 && Is("undefined")))
                        Fail(); // its results are local variables
                    Add(id, kind == NodeKind::WaitTill ? Leaf(NodeKind::Local) : Expression());
                }
                Expect(")");
                return Close(id);
            }

            // ---- expressions
            NodeId Expression(const int minimum = 1)
            {
                auto left = Unary();
                while (Peek().kind == Kind::Punct)
                {
                    const int precedence = Precedence(Peek().text);
                    if (precedence < minimum)
                        break;
                    const auto id = OpenAt(NodeKind::Binary, left);
                    pos_++;
                    Add(id, left);
                    Add(id, Expression(precedence + 1));
                    left = Close(id);
                }
                if (dialect_ == Dialect::Bo2 && minimum == 1 && Is("?"))
                {
                    const auto id = OpenAt(NodeKind::Conditional, left);
                    pos_++;
                    Add(id, left);
                    Add(id, Expression());
                    Expect(":");
                    Add(id, Expression());
                    left = Close(id);
                }
                return left;
            }

            NodeId Unary()
            {
                if (Is("!") || Is("~"))
                {
                    const auto id = Open(Is("!") ? NodeKind::Not : NodeKind::Complement);
                    pos_++;
                    Add(id, Unary());
                    return Close(id);
                }
                return Primary();
            }

            bool AtCallee() const
            {
                if (Is("thread") || (Is("[") && Is("[", 1)))
                    return true;
                if (IsIdent() && Is("(", 1))
                    return true;
                return IsPath() && Is("::", 1) && IsIdent(2) && Is("(", 3);
            }

            NodeId Callee()
            {
                const auto id = Open(NodeKind::Callee);
                if (Accept("["))
                {
                    Expect("[");
                    Add(id, Expression());
                    Expect("]");
                    Expect("]");
                    return Close(id);
                }
                if (IsPath() && Is("::", 1))
                {
                    ast_.nodes[id].qualifier = Peek().token + 1;
                    pos_ += 2;
                }
                if (!IsIdent())
                    Fail();
                ast_.nodes[id].token = Peek().token;
                pos_++;
                return Close(id);
            }

            NodeId Call(const std::optional<NodeId> object)
            {
                const auto id = object ? OpenAt(NodeKind::Call, *object) : Open(NodeKind::Call);
                ast_.nodes[id].method = object.has_value();
                ast_.nodes[id].threaded = Accept("thread");
                const auto callee = Callee();
                ast_.nodes[id].token = ast_.nodes[callee].token;
                Add(id, callee);
                if (object)
                    Add(id, *object);
                Expect("(");
                Arguments(id);
                return Close(id);
            }

            void Arguments(const NodeId id)
            {
                if (Accept(")"))
                    return;
                do
                    Add(id, Expression());
                while (Accept(","));
                Expect(")");
            }

            NodeId Primary()
            {
                auto node = Atom();
                for (;;)
                {
                    if (Is(".") && Peek(1).kind == Kind::Ident)
                    {
                        // ".size" is one word to T4; "x. size" is a field named size
                        const bool size = Peek(1).text == "size" && tokens_[Peek(1).token].pre.empty();
                        if (!size && !IsIdent(1))
                            Fail();
                        const auto id = OpenAt(size ? NodeKind::Size : NodeKind::Field, node);
                        Add(id, node);
                        pos_++;
                        ast_.nodes[id].token = Peek().token;
                        pos_++;
                        node = Close(id);
                    }
                    else if (AtCallee())
                        node = Call(node);
                    else if (Is("["))
                    {
                        const auto id = OpenAt(NodeKind::Index, node);
                        Add(id, node);
                        pos_++;
                        Add(id, Expression());
                        Expect("]");
                        node = Close(id);
                    }
                    else
                        return node;
                }
            }

            NodeId Atom()
            {
                const auto& t = Peek();
                if (t.kind == Kind::Number)
                    return Leaf(t.text.find('.') == std::string_view::npos ? NodeKind::Int : NodeKind::Float);
                if (t.kind == Kind::String)
                    return Leaf(t.text[0] == '&' ? NodeKind::LocalizedString : NodeKind::String);
                if (t.kind == Kind::Punct)
                    return Punctuated();
                if (AtCallee())
                    return Call(std::nullopt);
                if (IsPath() && Is("::", 1) && IsIdent(2))
                {
                    const auto path = t.token;
                    const auto id = Leaf(NodeKind::FunctionRef, 3);
                    ast_.nodes[id].qualifier = path + 1;
                    return id;
                }
                if (IsIdent())
                    return Leaf(NodeKind::Local);
                static constexpr std::array<std::pair<std::string_view, NodeKind>, 7> words{{{"undefined", NodeKind::Undefined},
                                                                                            {"self", NodeKind::Self},
                                                                                            {"level", NodeKind::Level},
                                                                                            {"game", NodeKind::Game},
                                                                                            {"anim", NodeKind::Anim},
                                                                                            {"true", NodeKind::True},
                                                                                            {"false", NodeKind::False}}};
                for (const auto& [word, kind] : words)
                    if (Is(word))
                        return Leaf(kind);
                Fail();
            }

            NodeId Punctuated()
            {
                if (Is("("))
                {
                    const auto id = Open(NodeKind::List);
                    pos_++;
                    Arguments(id);
                    return Close(id);
                }
                if (AtCallee())
                    return Call(std::nullopt);
                if (Is("[") && Is("]", 1))
                    return Leaf(NodeKind::EmptyArray, 2);
                if (Is("-") && Peek(1).kind == Kind::Number)
                    return Leaf(Peek(1).text.find('.') == std::string_view::npos ? NodeKind::Int : NodeKind::Float, 2);
                if (Is("%") && Peek(1).kind == Kind::Ident)
                    return Leaf(NodeKind::AnimRef, dialect_ == Dialect::Bo2 && Is("::", 2) && Peek(3).kind == Kind::Ident ? 4 : 2);
                if (Is("#") && Is("animtree", 1))
                    return Leaf(NodeKind::AnimTree, 2);
                if (Is("::") && IsIdent(1))
                    return Leaf(NodeKind::FunctionRef, 2);
                Fail();
            }

            const std::vector<Token>& tokens_;
            std::vector<Lexeme> lex_;
            Dialect dialect_;
            Ast ast_;
            size_t pos_ = 0;
        };

        void Dump(const Ast& ast, const std::vector<Token>& tokens, const NodeId id, std::string& out)
        {
            const auto& n = ast[id];
            out += '(';
            out += NodeKindName(n.kind);
            const auto text = [&](const uint32_t token)
            {
                out += ' ';
                out += tokens[token].text;
            };
            switch (n.kind)
            {
            case NodeKind::Function:
            case NodeKind::Param:
            case NodeKind::Assign:
            case NodeKind::Binary:
            case NodeKind::String:
            case NodeKind::LocalizedString:
            case NodeKind::Local:
            case NodeKind::Field:
            case NodeKind::AnimRef:
                text(n.token);
                break;
            case NodeKind::Int:
            case NodeKind::Float:
                if (n.begin != n.token)
                    out += " -";
                text(n.token);
                break;
            case NodeKind::FunctionRef:
            case NodeKind::Callee:
                if (n.qualifier)
                    text(n.qualifier - 1);
                if (n.kids.empty())
                    text(n.token);
                break;
            case NodeKind::Call:
                if (n.threaded)
                    out += " thread";
                if (n.method)
                    out += " method";
                break;
            default:
                break;
            }
            for (const auto kid : n.kids)
            {
                out += ' ';
                Dump(ast, tokens, kid, out);
            }
            out += ')';
        }
    } // namespace

    const char* NodeKindName(const NodeKind kind)
    {
        static constexpr const char* names[]{
            "script", "directive", "devbegin", "devend",     "func",     "param",    "block",    "do",       "set",      "inc",           "dec",
            "return", "wait",      "waittill", "waittillmatch", "waittillframeend", "notify",  "endon",    "break",    "continue", "breakpoint", "prof",
            "if",     "while",     "for",      "foreach",  "dowhile",  "switch",     "case",     "default",  "dev",      "empty",    "bin",      "cond",     "not",           "compl",
            "undefined", "self",   "level",    "game",       "anim",     "true",     "false",    "int",      "float",    "str",           "istr",
            "list",   "array",     "var",      "field",      "size",     "index",    "ref",      "animref",  "animtree", "call",          "callee"};
        static_assert(std::size(names) == static_cast<size_t>(NodeKind::Callee) + 1);
        return names[static_cast<size_t>(kind)];
    }

    std::vector<NodeId> Ast::Functions() const
    {
        std::vector<NodeId> out;
        for (const auto id : nodes[root].kids)
            if (nodes[id].kind == NodeKind::Function)
                out.push_back(id);
        return out;
    }

    Ast ParseAst(const std::vector<Token>& tokens, const Dialect dialect)
    {
        return Parser(tokens, dialect).Run();
    }

    std::string DumpAst(const Ast& ast, const std::vector<Token>& tokens)
    {
        std::string out;
        for (const auto kid : ast[ast.root].kids)
        {
            Dump(ast, tokens, kid, out);
            out += '\n';
        }
        return out;
    }
} // namespace w2b::gsc

#include "vm_rules.h"

#include "types.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <set>

namespace w2b::gsc::port
{
    namespace
    {
        // how many turns of a watched loop pass between two looks at the clock
        constexpr const char* TURNS = "100";

        const std::string& Text(const std::vector<Token>& tokens, const Ast& ast, const NodeId id)
        {
            return tokens[ast[id].token].text;
        }

        bool IsNumber(const Ast& ast, const NodeId id)
        {
            return ast[id].kind == NodeKind::Int || ast[id].kind == NodeKind::Float;
        }

        // "( x )" -> x
        NodeId Unwrap(const Ast& ast, NodeId id)
        {
            while (ast[id].kind == NodeKind::List && ast[id].kids.size() == 1)
                id = ast[id].kids[0];
            return id;
        }

        class Function
        {
        public:
            Function(const Ast& ast, const std::vector<Token>& tokens, const NodeId function) : ast_(ast), tokens_(tokens), function_(function)
            {
            }

            bool Bounded(const NodeId loop) const
            {
                const auto& n = ast_[loop];
                if (n.kind == NodeKind::ForEach)
                    return true;
                const auto body = n.kids.back();
                if (AlwaysWaits(body))
                    return true;
                return n.kind == NodeKind::For && Counts(n);
            }

        private:
            // ---- waits

            // whether every turn that gets to the end of `id`, or to a "continue", has waited
            bool AlwaysWaits(const NodeId id) const
            {
                const auto& n = ast_[id];
                switch (n.kind)
                {
                case NodeKind::Block:
                    for (const auto kid : n.kids)
                    {
                        if (AlwaysWaits(kid))
                            return true;
                        if (Continues(kid))
                            return false;
                    }
                    return false;
                case NodeKind::If:
                    return n.kids.size() == 3 && AlwaysWaits(n.kids[1]) && AlwaysWaits(n.kids[2]);
                case NodeKind::Wait:
                {
                    // a wait on an undefined value does not wait
                    const auto seconds = Unwrap(ast_, n.kids[0]);
                    return IsNumber(ast_, seconds) && ast_[seconds].begin == ast_[seconds].token
                           && std::strtod(Text(tokens_, ast_, seconds).c_str(), nullptr) > 0;
                }
                case NodeKind::WaitTill:
                    return (ast_[n.kids[0]].kind == NodeKind::Level || ast_[n.kids[0]].kind == NodeKind::Self)
                           && ast_[Unwrap(ast_, n.kids[1])].kind == NodeKind::String;
                default:
                    return false;
                }
            }

            bool Continues(const NodeId id) const
            {
                const auto& n = ast_[id];
                if (n.kind == NodeKind::Continue)
                    return true;
                if (n.kind == NodeKind::For || n.kind == NodeKind::While || n.kind == NodeKind::ForEach || n.kind == NodeKind::DoWhile)
                    return false; // its own loop's
                return std::any_of(n.kids.begin(), n.kids.end(), [this](const NodeId kid) { return Continues(kid); });
            }

            // ---- counted loops: "for ( i = <number>; i < <number>; i++ )"

            bool Counts(const Node& loop) const
            {
                const auto init = loop.kids[0], condition = Unwrap(ast_, loop.kids[1]), step = loop.kids[2], body = loop.kids[3];
                if (ast_[init].kind != NodeKind::Assign || Text(tokens_, ast_, init) != "=" || ast_[ast_[init].kids[0]].kind != NodeKind::Local)
                    return false;
                const auto& variable = Text(tokens_, ast_, ast_[init].kids[0]);
                if (ast_[condition].kind != NodeKind::Binary || ast_[ast_[condition].kids[0]].kind != NodeKind::Local
                    || Text(tokens_, ast_, ast_[condition].kids[0]) != variable)
                    return false;
                const auto up = Direction(step, variable);
                if (!up)
                    return false;
                const auto& op = Text(tokens_, ast_, condition);
                if (*up ? op != "<" && op != "<=" : op != ">" && op != ">=")
                    return false;
                std::set<std::string> arrays;
                if (!Number(ast_[init].kids[1], loop, arrays) || !Number(ast_[condition].kids[1], loop, arrays))
                    return false;
                if (Assigned(body, variable))
                    return false;
                return std::none_of(arrays.begin(), arrays.end(), [&](const std::string& name) { return Assigned(body, name); });
            }

            std::optional<bool> Direction(const NodeId step, const std::string& variable) const
            {
                const auto& n = ast_[step];
                if (n.kids.empty() || ast_[n.kids[0]].kind != NodeKind::Local || Text(tokens_, ast_, n.kids[0]) != variable)
                    return std::nullopt;
                if (n.kind == NodeKind::Increment)
                    return true;
                if (n.kind == NodeKind::Decrement)
                    return false;
                if (n.kind != NodeKind::Assign)
                    return std::nullopt;
                const auto& op = Text(tokens_, ast_, step);
                const auto amount = Unwrap(ast_, n.kids[1]);
                const bool positive = IsNumber(ast_, amount) && ast_[amount].begin == ast_[amount].token
                                      && std::strtod(Text(tokens_, ast_, amount).c_str(), nullptr) > 0;
                if (!positive || (op != "+=" && op != "-="))
                    return std::nullopt;
                return op == "+=";
            }

            // whether `id` is a number whatever happens; the arrays whose sizes it reads
            bool Number(NodeId id, const Node& loop, std::set<std::string>& arrays) const
            {
                id = Unwrap(ast_, id);
                const auto& n = ast_[id];
                if (IsNumber(ast_, id))
                    return true;
                if (n.kind == NodeKind::Binary)
                {
                    const auto& op = tokens_[n.token].text;
                    return (op == "+" || op == "-" || op == "*") && Number(n.kids[0], loop, arrays) && Number(n.kids[1], loop, arrays);
                }
                if (n.kind != NodeKind::Size)
                    return false;
                const auto owner = Unwrap(ast_, n.kids[0]);
                if (ArrayCall(owner))
                    return true;
                if (ast_[owner].kind != NodeKind::Local)
                    return false;
                const auto& name = Text(tokens_, ast_, owner);
                if (!ArrayVariable(name, loop))
                    return false;
                arrays.insert(name);
                return true;
            }

            // a builtin that returns an array always
            bool ArrayCall(const NodeId id) const
            {
                const auto& n = ast_[id];
                if (n.kind != NodeKind::Call || n.threaded || !ast_[n.kids[0]].kids.empty())
                    return false;
                static constexpr std::array<std::string_view, 9> names{"getentarray", "getarraykeys", "getnodearray", "getaiarray", "getaispeciesarray",
                                                                       "strtok",      "getplayers",   "get_players",  "getspawnerarray"};
                const auto low = tokens_[n.token].Low();
                return std::find(names.begin(), names.end(), low) != names.end();
            }

            bool ArrayVariable(const std::string& name, const Node& loop) const
            {
                bool only = true, before = false;
                for (const auto kid : ast_[function_].kids)
                    if (ast_[kid].kind == NodeKind::Param && Text(tokens_, ast_, kid) == name)
                        only = false;
                Walk(function_,
                     [&](const NodeId id)
                     {
                         const auto& n = ast_[id];
                         if (n.kind == NodeKind::WaitTill)
                             for (size_t k = 2; k < n.kids.size(); k++)
                                 only = only && Text(tokens_, ast_, n.kids[k]) != name;
                         if (n.kind == NodeKind::ForEach)
                             for (size_t k = 0; k + 2 < n.kids.size(); k++)
                                 only = only && Text(tokens_, ast_, n.kids[k]) != name;
                         if (n.kids.empty() || ast_[n.kids[0]].kind != NodeKind::Local || Text(tokens_, ast_, n.kids[0]) != name)
                             return;
                         if (n.kind == NodeKind::Increment || n.kind == NodeKind::Decrement)
                             only = false;
                         if (n.kind != NodeKind::Assign)
                             return;
                         const auto value = Unwrap(ast_, n.kids[1]);
                         if (tokens_[n.token].text != "=" || (ast_[value].kind != NodeKind::EmptyArray && !ArrayCall(value)))
                             only = false;
                         else if (n.end <= loop.begin && Dominates(id, loop.begin))
                             before = true;
                     });
                return only && before;
            }

            // whether the statement always runs before token `at`: the block it stands in encloses `at`
            bool Dominates(const NodeId statement, const uint32_t at) const
            {
                bool found = false;
                Walk(function_,
                     [&](const NodeId id)
                     {
                         const auto& n = ast_[id];
                         if (n.kind != NodeKind::Block || at < n.begin || at >= n.end)
                             return;
                         for (const auto kid : n.kids)
                         {
                             if (ast_[kid].kind == NodeKind::Case || ast_[kid].kind == NodeKind::Default)
                                 return; // a switch's block is entered in the middle
                             found = found || kid == statement;
                         }
                     });
                return found;
            }

            bool Assigned(const NodeId root, const std::string& name) const
            {
                bool found = false;
                Walk(root,
                     [&](const NodeId id)
                     {
                         const auto& n = ast_[id];
                         if (n.kind == NodeKind::WaitTill)
                             for (size_t k = 2; k < n.kids.size(); k++)
                                 found = found || Text(tokens_, ast_, n.kids[k]) == name;
                         if (n.kind == NodeKind::ForEach)
                             for (size_t k = 0; k + 2 < n.kids.size(); k++)
                                 found = found || Text(tokens_, ast_, n.kids[k]) == name;
                         if (n.kind != NodeKind::Assign && n.kind != NodeKind::Increment && n.kind != NodeKind::Decrement)
                             return;
                         auto target = n.kids[0];
                         while (ast_[target].kind == NodeKind::Index || ast_[target].kind == NodeKind::Field)
                             target = ast_[target].kids[0];
                         found = found || (ast_[target].kind == NodeKind::Local && Text(tokens_, ast_, target) == name);
                     });
                return found;
            }

            void Walk(const NodeId root, const std::function<void(NodeId)>& visit) const
            {
                std::vector<NodeId> stack{root};
                while (!stack.empty())
                {
                    const auto id = stack.back();
                    stack.pop_back();
                    visit(id);
                    stack.insert(stack.end(), ast_[id].kids.begin(), ast_[id].kids.end());
                }
            }

            const Ast& ast_;
            const std::vector<Token>& tokens_;
            NodeId function_;
        };

        void Before(std::vector<Token>& tokens, const uint32_t at, const std::string& text)
        {
            tokens[at].pre = text + tokens[at].pre;
        }
    } // namespace

    bool LoopIsBounded(const Ast& ast, const std::vector<Token>& tokens, const NodeId function, const NodeId loop)
    {
        return Function(ast, tokens, function).Bounded(loop);
    }

    namespace
    {
        // "[[" and "]]" as single brackets, so that every node ends between two tokens; the text stays the same
        void SplitBrackets(std::vector<Token>& tokens)
        {
            std::vector<Token> out;
            out.reserve(tokens.size() + 16);
            for (auto& token : tokens)
            {
                if (token.kind != Kind::Punct || (token.text != "[[" && token.text != "]]"))
                {
                    out.push_back(std::move(token));
                    continue;
                }
                const std::string half(1, token.text[0]);
                out.push_back(Token{Kind::Punct, half, token.pre, token.line});
                out.push_back(Token{Kind::Punct, half, "", token.line});
            }
            tokens = std::move(out);
        }

        class Rules
        {
        public:
            Rules(std::vector<Token>& tokens, const VmRuntime& runtime) : tokens_(tokens), ast_(ParseAst(tokens, Dialect::Bo2)), types_(ast_, tokens_), runtime_(runtime.script + "::")
            {
            }

            // Edits go from the inside out and from expressions to statements: text
            // put before a token lands before what is already there.
            VmCounts Run()
            {
                for (const auto function : ast_.Functions())
                {
                    function_ = function;
                    Expressions(function, false, NO_NODE);
                }
                for (const auto function : ast_.Functions())
                    Loops(function);
                return counts_;
            }

        private:
            void Wrap(const NodeId id, const std::string& before, const std::string& after)
            {
                Before(tokens_, ast_[id].begin, before);
                Before(tokens_, ast_[id].end, after);
            }

            const std::string& Operator(const NodeId id) const
            {
                return tokens_[ast_[id].token].text;
            }

            bool IsLogic(const NodeId id) const
            {
                return ast_[id].kind == NodeKind::Binary && (Operator(id) == "&&" || Operator(id) == "||");
            }

            // `condition`: only the truth of the expression is used
            void Expressions(const NodeId id, const bool condition, const NodeId parent)
            {
                const auto& n = ast_[id];
                for (size_t k = 0; k < n.kids.size(); k++)
                {
                    bool truth = false;
                    switch (n.kind)
                    {
                    case NodeKind::If:
                    case NodeKind::While:
                        truth = k == 0;
                        break;
                    case NodeKind::For:
                    case NodeKind::DoWhile:
                        truth = k == 1;
                        break;
                    case NodeKind::Conditional:
                        truth = k == 0;
                        break;
                    case NodeKind::Not:
                        truth = true;
                        break;
                    case NodeKind::List:
                        truth = condition && n.kids.size() == 1;
                        break;
                    case NodeKind::Binary:
                        // the left side of a logical operator is only tested; the right side is its value
                        truth = IsLogic(id) && (condition || k == 0);
                        break;
                    default:
                        break;
                    }
                    Expressions(n.kids[k], truth, id);
                }
                switch (n.kind)
                {
                case NodeKind::Binary:
                    if (IsLogic(id))
                    {
                        if (!condition)
                            Logic(id);
                    }
                    else
                        Comparison(id);
                    break;
                case NodeKind::List:
                    Vector(id, parent);
                    break;
                case NodeKind::Switch:
                    Switch(id);
                    break;
                default:
                    break;
                }
            }

            // T4's "&&" and "||" make 0 or 1 of the right side; T6's are the right side itself.
            void Logic(const NodeId id)
            {
                const auto right = ast_[id].kids[1];
                const auto value = Unwrap(ast_, right);
                if (IsLogic(value) || IsTruth(value))
                    return; // its own right side is made 0 or 1
                counts_.logic++;
                Wrap(right, " !( !(", " ) )");
            }

            bool IsTruth(const NodeId id) const
            {
                const auto& n = ast_[id];
                if (n.kind == NodeKind::Not || n.kind == NodeKind::True || n.kind == NodeKind::False)
                    return true;
                if (n.kind == NodeKind::Binary)
                {
                    const auto& op = Operator(id);
                    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
                }
                return n.kind == NodeKind::Call && tokens_[n.token].Low() == "isdefined";
            }

            // Comparisons whose sides may be two strings or two arrays. A side must be known to take
            // that type, the other to allow it: comparisons of values nothing is known about stay as they are.
            void Comparison(const NodeId id)
            {
                const auto& n = ast_[id];
                const auto& op = Operator(id);
                const bool order = op == "<" || op == ">" || op == "<=" || op == ">=";
                if (!order && op != "==" && op != "!=")
                    return;
                const auto type = order ? TYPE_STRING : TYPE_ARRAY;
                const auto a = types_.Of(function_, n.kids[0]), b = types_.Of(function_, n.kids[1]);
                if (!(a.Known(type) && b.May(type)) && !(b.Known(type) && a.May(type)))
                    return;
                if (order)
                {
                    counts_.comparisons++;
                    Call(id, op == "<" ? "waw_less" : op == ">" ? "waw_greater" : op == "<=" ? "waw_less_equal" : "waw_greater_equal");
                }
                else
                {
                    counts_.arrays++;
                    Call(id, op == "==" ? "waw_equal" : "waw_not_equal");
                }
            }

            // "a op b" -> "function( a, b )"
            void Call(const NodeId id, const char* function)
            {
                tokens_[ast_[id].token].text = ",";
                Wrap(id, " " + runtime_ + function + "(", " )");
            }

            // A vector part that is not a number is a fault in both games. T4 gives undefined;
            // T6 gives garbage and leaves its stack wrong, which stops threads later on.
            void Vector(const NodeId id, const NodeId parent)
            {
                const auto& n = ast_[id];
                if (n.kids.size() != 3 || std::all_of(n.kids.begin(), n.kids.end(), [this](const NodeId kid) { return IsNumeric(kid); }))
                    return;
                const auto above = parent == NO_NODE ? NodeKind::Empty : ast_[parent].kind;
                if ((above == NodeKind::Index || above == NodeKind::Size || above == NodeKind::Field) && ast_[parent].kids[0] == id)
                    return; // T6 does not index a call
                counts_.vectors++;
                Before(tokens_, n.begin, " " + runtime_ + "waw_vector");
            }

            bool IsNumeric(const NodeId id) const
            {
                const auto value = Unwrap(ast_, id);
                const auto& n = ast_[value];
                if (IsNumber(ast_, value))
                    return true;
                if (n.kind == NodeKind::Binary)
                {
                    const auto& op = Operator(value);
                    return (op == "+" || op == "-" || op == "*" || op == "/") && IsNumeric(n.kids[0]) && IsNumeric(n.kids[1]);
                }
                if (n.kind != NodeKind::Call || n.method || n.threaded || !ast_[n.kids[0]].kids.empty() || ast_[n.kids[0]].qualifier)
                    return false;
                const auto name = tokens_[n.token].Low();
                if (name != "randomint" && name != "randomintrange" && name != "randomfloat" && name != "randomfloatrange")
                    return false;
                return std::all_of(n.kids.begin() + 1, n.kids.end(), [this](const NodeId kid) { return IsNumeric(kid); });
            }

            // T4 runs the default label for a value it cannot switch on; T6 skips the whole switch.
            void Switch(const NodeId id)
            {
                const auto& n = ast_[id];
                const auto& block = ast_[n.kids[1]];
                const bool hasDefault = std::any_of(block.kids.begin(), block.kids.end(), [this](const NodeId kid) { return ast_[kid].kind == NodeKind::Default; });
                const auto kind = ast_[n.kids[0]].kind;
                if (!hasDefault || kind == NodeKind::Int || kind == NodeKind::String)
                    return;
                counts_.switches++;
                Wrap(n.kids[0], " " + runtime_ + "waw_switch(", " )");
            }

            void Loops(const NodeId function)
            {
                const Function analysis(ast_, tokens_, function);
                std::vector<NodeId> watched, stack{function};
                while (!stack.empty())
                {
                    const auto id = stack.back();
                    stack.pop_back();
                    const auto& n = ast_[id];
                    if (n.kind == NodeKind::For || n.kind == NodeKind::While || n.kind == NodeKind::ForEach)
                    {
                        counts_.loops++;
                        if (!analysis.Bounded(id))
                            watched.push_back(id);
                    }
                    stack.insert(stack.end(), n.kids.begin(), n.kids.end());
                }
                if (watched.empty())
                    return;
                counts_.watched += static_cast<int>(watched.size());
                const std::string check = " waw2bo2_turns++; if ( waw2bo2_turns > " + std::string(TURNS) + " ) { waw2bo2_turns = 0; " + runtime_ + "waw_watch( \""
                                          + tokens_[ast_[function].token].text + "\" ); }";
                for (const auto loop : watched)
                {
                    const auto& body = ast_[ast_[loop].kids.back()];
                    if (body.kind == NodeKind::Block)
                    {
                        Before(tokens_, body.begin + 1, check);
                        continue;
                    }
                    Before(tokens_, body.begin, " {" + check);
                    Before(tokens_, body.end, " }");
                }
                Before(tokens_, ast_[ast_[function].kids.back()].begin + 1, " waw2bo2_turns = 0;");
            }

            std::vector<Token>& tokens_;
            Ast ast_;
            TypeInference types_;
            std::string runtime_;
            NodeId function_ = NO_NODE;
            VmCounts counts_;
        };
    } // namespace

    void VmCounts::Add(const VmCounts& other)
    {
        loops += other.loops;
        watched += other.watched;
        switches += other.switches;
        vectors += other.vectors;
        logic += other.logic;
        arrays += other.arrays;
        comparisons += other.comparisons;
    }

    VmCounts ApplyVmRules(std::vector<Token>& tokens, const VmRuntime& runtime)
    {
        SplitBrackets(tokens);
        return Rules(tokens, runtime).Run();
    }
} // namespace w2b::gsc::port

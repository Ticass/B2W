#include "loops.h"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <set>

namespace w2b::gsc::port
{
    namespace
    {
        // Two counts of this many turns in one server frame. T4 measures 2.5 s
        // instead; its VM does about two million turns of an empty loop in that
        // time, and a body that does anything far fewer.
        constexpr const char* TURNS = "100000";

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
                if (n.kind == NodeKind::For || n.kind == NodeKind::While || n.kind == NodeKind::ForEach)
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

    LoopCounts WatchLoops(std::vector<Token>& tokens, const std::string& killer)
    {
        LoopCounts counts;
        const auto ast = ParseAst(tokens, Dialect::Bo2);
        for (const auto function : ast.Functions())
        {
            const Function analysis(ast, tokens, function);
            std::vector<NodeId> watched, stack{function};
            while (!stack.empty())
            {
                const auto id = stack.back();
                stack.pop_back();
                const auto& n = ast[id];
                if (n.kind == NodeKind::For || n.kind == NodeKind::While || n.kind == NodeKind::ForEach)
                {
                    counts.loops++;
                    if (!analysis.Bounded(id))
                        watched.push_back(id);
                }
                stack.insert(stack.end(), n.kids.begin(), n.kids.end());
            }
            if (watched.empty())
                continue;
            counts.watched += static_cast<int>(watched.size());
            const std::string check = " waw2bo2_turns++; if ( waw2bo2_turns > " + std::string(TURNS)
                                      + " ) { if ( isdefined( level.waw_running ) && isdefined( waw2bo2_frame ) && waw2bo2_frame == gettime() ) " + killer + "( \""
                                      + tokens[ast[function].token].text + "\" ); waw2bo2_frame = gettime(); waw2bo2_turns = 0; }";
            for (const auto loop : watched)
            {
                const auto& body = ast[ast[loop].kids.back()];
                if (body.kind == NodeKind::Block)
                {
                    Before(tokens, body.begin + 1, check);
                    continue;
                }
                Before(tokens, body.begin, " {" + check);
                Before(tokens, body.end, " }");
            }
            Before(tokens, ast[ast[function].kids.back()].begin + 1, " waw2bo2_turns = 0;");
        }
        return counts;
    }

    int LowerSwitches(std::vector<Token>& tokens, const std::string& reader)
    {
        int changed = 0;
        const auto ast = ParseAst(tokens, Dialect::Bo2);
        for (NodeId id = 0; id < ast.nodes.size(); id++)
        {
            const auto& n = ast[id];
            if (n.kind != NodeKind::Switch)
                continue;
            const auto& block = ast[n.kids[1]];
            const bool hasDefault = std::any_of(block.kids.begin(), block.kids.end(), [&](const NodeId kid) { return ast[kid].kind == NodeKind::Default; });
            const auto& value = ast[n.kids[0]];
            if (!hasDefault || value.kind == NodeKind::Int || value.kind == NodeKind::String)
                continue;
            Before(tokens, value.begin, " " + reader + "(");
            Before(tokens, value.end, " )");
            changed++;
        }
        return changed;
    }
} // namespace w2b::gsc::port

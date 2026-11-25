#pragma once
// The syntax tree of a WaW script. The grammar is T4's own (docs/T4_SCRIPT_LANGUAGE.md):
// a script this parser accepts is one WaW compiles, and the tree has WaW's shape.
// Nodes refer to the tokens they were read from, so edits stay token edits and
// untouched text is emitted as it was.

#include "tokens.h"

#include <cstdint>
#include <string>
#include <vector>

namespace w2b::gsc
{
    enum class NodeKind : uint8_t
    {
        // top level: the kids of the root
        Script,
        Directive, // #include, #using_animtree, ...
        DevBegin,  // "/#" between functions
        DevEnd,
        Function, // token = name; kids = Param..., Block
        Param,

        // statements
        Block,
        ExprStmt,  // kids = Call
        Assign,    // token = "=", "+=", ...; kids = target, value
        Increment, // kids = target
        Decrement,
        Return,        // kids = value or none
        Wait,          // kids = seconds
        WaitTill,      // kids = object, event, Local...
        WaitTillMatch, // kids = object, event, value...
        WaitTillFrameEnd,
        Notify, // kids = object, event, value...
        EndOn,  // kids = object, event
        Break,
        Continue,
        Breakpoint,
        Prof,   // prof_begin, prof_end
        If,     // kids = condition, then, [else]
        While,  // kids = condition, body
        For,    // kids = init, condition, step, body; an absent part is an Empty
        ForEach, // BO2 only. kids = Local, [Local], array, body
        Switch, // kids = value, Block
        Case,   // kids = value
        Default,
        DevBlock, // "/# ... #/"
        Empty,    // ";" or an absent part of a for

        // expressions
        Binary, // token = operator; kids = left, right
        Not,
        Complement,
        Undefined,
        Self,
        Level,
        Game,
        Anim,
        True,
        False,
        Int, // a leading "-" belongs to the literal
        Float,
        String,
        LocalizedString,
        List, // "( a )" and "( x, y, z )"
        EmptyArray,
        Local,
        Field,       // token = field name; kids = object
        Size,        // ".size"
        Index,       // kids = object, key
        FunctionRef, // "::name", "path::name"; token = name
        AnimRef,     // "%name"
        AnimTree,    // "#animtree"
        Call,        // kids = callee, [object], args...
        Callee,      // token = name, qualifier = path; or kids = the expression of "[[ e ]]"
    };

    const char* NodeKindName(NodeKind kind);

    using NodeId = uint32_t;
    inline constexpr NodeId NO_NODE = ~0u;

    struct Node
    {
        NodeKind kind = NodeKind::Empty;
        bool threaded = false;  // Call: "thread"
        bool method = false;    // Call: kids[1] is the object
        uint32_t token = 0;     // the token that names it
        uint32_t qualifier = 0; // Callee/FunctionRef: path token + 1, 0 = none
        uint32_t begin = 0;     // tokens [begin, end)
        uint32_t end = 0;
        std::vector<NodeId> kids;
    };

    struct Ast
    {
        std::vector<Node> nodes;
        NodeId root = NO_NODE;

        const Node& operator[](const NodeId id) const
        {
            return nodes[id];
        }
        std::vector<NodeId> Functions() const;
    };

    enum class Dialect : uint8_t
    {
        Waw,
        Bo2, // translated scripts: the converter's own code also has foreach
    };

    // Throws GscSyntaxError where WaW reports "bad syntax".
    Ast ParseAst(const std::vector<Token>& tokens, Dialect dialect = Dialect::Waw);

    // one line per top-level node, for comparing trees
    std::string DumpAst(const Ast& ast, const std::vector<Token>& tokens);
} // namespace w2b::gsc

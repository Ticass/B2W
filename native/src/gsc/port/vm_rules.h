#pragma once
// What T4's script VM does and T6's does not, as source rewrites
// (docs/T4_SCRIPT_LANGUAGE.md, "Measured differences between the two VMs").

#include "gsc/ast.h"

#include <string>
#include <vector>

namespace w2b::gsc::port
{
    // Where the compat functions the rewrites call live:
    // "<runtime>::waw_watch", "::waw_switch", "::waw_vector", "::waw_equal", "::waw_not_equal",
    // "::waw_less", "::waw_greater", "::waw_less_equal", "::waw_greater_equal"
    struct VmRuntime
    {
        std::string script;
    };

    struct VmCounts
    {
        int loops = 0;       // all loops
        int watched = 0;     // loops that got the watchdog
        int switches = 0;    // default label for a value that cannot match
        int vectors = 0;     // a part that may not be a number
        int logic = 0;       // "a && b" used as a value
        int arrays = 0;      // array == array
        int comparisons = 0; // string < string

        int Changes() const
        {
            return watched + switches + vectors + logic + arrays + comparisons;
        }
        void Add(const VmCounts& other);
    };

    // Whether the loop is known to end, or to wait on every turn.
    bool LoopIsBounded(const Ast& ast, const std::vector<Token>& tokens, NodeId function, NodeId loop);

    // Throws GscSyntaxError for a script the grammar does not accept.
    VmCounts ApplyVmRules(std::vector<Token>& tokens, const VmRuntime& runtime);
} // namespace w2b::gsc::port

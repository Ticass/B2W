#pragma once
// T4's loop watchdog, for T6 (docs/T4_SCRIPT_LANGUAGE.md, "Faults and the loop watchdog").
// A bad operation gives undefined in both games and the script goes on, so
// "i < undefined.size" or "while ( undefined )" never ends. T4 kills a thread
// that loops for 2.5 s without a wait; T6 hangs.

#include "gsc/ast.h"

#include <string>

namespace w2b::gsc::port
{
    struct LoopCounts
    {
        int loops = 0;   // all loops
        int watched = 0; // loops that got the watchdog
    };

    // Whether the loop is known to end, or to wait on every turn.
    bool LoopIsBounded(const Ast& ast, const std::vector<Token>& tokens, NodeId function, NodeId loop);

    // Adds the watchdog to every loop that is not bounded. `killer` is called
    // with the function's name where T4 kills the thread.
    LoopCounts WatchLoops(std::vector<Token>& tokens, const std::string& killer);

    // T4 runs the default label for a value it cannot switch on (undefined, a
    // float, an object); T6 skips the whole switch. The value goes through `reader`.
    int LowerSwitches(std::vector<Token>& tokens, const std::string& reader);
} // namespace w2b::gsc::port

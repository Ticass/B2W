#pragma once
// gscport's statement helpers: top_level_statements, statement_locals,
// local_dependencies, script_branch_taken.

#include "util.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace w2b::gsc::port
{
    using Span = std::pair<long long, long long>;

    std::vector<Span> TopLevelStatements(const Tokens& tokens, long long open, long long close);
    std::pair<std::set<std::string>, std::set<std::string>> StatementLocals(const Tokens& tokens, long long start, long long end);
    std::set<size_t> LocalDependencies(const Tokens& tokens, const std::vector<Span>& spans, const std::set<size_t>& keep);
    bool ScriptBranchTaken(const Tokens& tokens, long long bodyOpen, long long bodyClose, long long index, const std::optional<std::string>& mapName);
} // namespace w2b::gsc::port

#pragma once

#include "models/models.h"

#include <map>
#include <utility>
#include <vector>

namespace w2b::models
{
    using ModelNames = std::map<std::string, std::string, std::less<>>;
    using RenameList = std::vector<std::pair<std::string, std::string>>; // old -> new, in discovery order

    std::string RewriteModelLiterals(std::string_view source, const ModelNames& renamed);
    void RewriteStagedScripts(const Path& project, const std::function<std::string(std::string_view)>& transform);
    util::Json RenamesJson(const RenameList& renamed);
    // Gives every converted model a name of its own; returns the renames.
    RenameList IsolateModelNames(const Path& project);
}

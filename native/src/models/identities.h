#pragma once

#include "models/models.h"

#include <map>

namespace w2b::models
{
    using ModelNames = std::map<std::string, std::string, std::less<>>;

    std::string RewriteModelLiterals(std::string_view source, const ModelNames& renamed);
    void RewriteStagedScripts(const Path& project, const std::function<std::string(std::string_view)>& transform);
    Json IsolateModelNames(const Path& project);
    Json IsolateModelNamesRequest(const Path& request);
}

#pragma once

#include "core/pytext.h"

#include <filesystem>
#include <optional>

namespace w2b::gsc::port
{
    namespace fs = std::filesystem;

    struct ScriptTemplate
    {
        fs::path root;
        std::string name;
    };

    struct StageDiagnostics
    {
        std::vector<std::string> errors, warnings;
    };

    void StageTemplateScripts(const fs::path& root, std::string_view project, const ScriptTemplate& source,
                              StageDiagnostics& diagnostics);
    void StageRequiredScripts(const fs::path& root, std::string_view project, const std::optional<ScriptTemplate>& source,
                              StageDiagnostics& diagnostics);
    void StageScriptRawfiles(const fs::path& root, const std::optional<fs::path>& bo2, StageDiagnostics& diagnostics);
    std::vector<std::string> MapScripts(const fs::path& root, std::string_view project);
    void RewriteTemplateReferences(const fs::path& root, std::string_view project, StageDiagnostics& diagnostics);
    py::Json StageScriptFilesRequest(const fs::path& request);
    py::Json MapScriptsRequest(const fs::path& request);
}

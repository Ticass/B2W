#include "gsc/port/staging.h"

#include "core/encoding.h"
#include "core/files.h"
#include "gsc/port/generators.h"
#include "gsc/port/util.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace w2b::gsc::port
{
    namespace
    {
        constexpr std::array<std::string_view, 3> ScriptSubfolders{"", "createfx", "createart"};
        constexpr std::array<std::string_view, 3> ScriptKinds{"", "_amb", "_fx"};
        constexpr std::array<std::pair<std::string_view, std::string_view>, 2> ScriptSides{{
            {"maps/mp", ".gsc"}, {"clientscripts/mp", ".csc"}
        }};

        std::string PosixPath(const fs::path& path)
        {
            auto text = files::Utf8(path);
            std::ranges::replace(text, '\\', '/');
            return text;
        }

        std::string WindowsPath(fs::path path)
        {
            return files::Utf8(path.make_preferred());
        }

        std::string ReadScript(const fs::path& path)
        {
            std::string bytes;
            if (!files::ReadBytes(path, bytes))
                throw std::runtime_error("cannot read " + WindowsPath(path));
            return encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
        }

        bool StartsWithName(std::wstring_view name, std::wstring_view prefix)
        {
            return name.size() >= prefix.size() && _wcsnicmp(name.data(), prefix.data(), prefix.size()) == 0;
        }

        void AppendScripts(std::vector<std::string>& found, const fs::path& root, const fs::path& folder,
                           std::string_view prefix, std::string_view suffix, bool sorted)
        {
            const auto namePrefix = files::FromUtf8(prefix).wstring();
            const auto nameSuffix = files::FromUtf8(suffix).wstring();
            std::vector<fs::path> paths;
            std::error_code error;
            for (fs::directory_iterator it(root / folder, error), end; !error && it != end; it.increment(error))
            {
                auto name = it->path().filename();
                const auto& text = name.native();
                if (text.size() >= namePrefix.size() + nameSuffix.size() && StartsWithName(text, namePrefix) &&
                    StartsWithName(std::wstring_view(text).substr(text.size() - nameSuffix.size()), nameSuffix))
                    paths.push_back(std::move(name));
            }
            if (sorted)
                std::stable_sort(paths.begin(), paths.end(), [](const fs::path& lhs, const fs::path& rhs)
                {
                    return _wcsicmp(lhs.c_str(), rhs.c_str()) < 0;
                });
            for (const auto& name : paths)
                found.push_back(PosixPath(folder / name));
        }

        std::optional<ScriptTemplate> GetTemplate(const Json& value)
        {
            if (value.is_null())
                return std::nullopt;
            return ScriptTemplate{files::FromUtf8(value.at("root").get<std::string>()), value.at("name").get<std::string>()};
        }
    }

    void StageTemplateScripts(const fs::path& root, std::string_view project, const ScriptTemplate& source,
                              StageDiagnostics& diagnostics)
    {
        auto paths = files::GlobSuffix(source.root, "sc");
        const auto prefix = files::FromUtf8(source.name).wstring();
        for (const auto& path : paths)
        {
            const auto rel = path.lexically_relative(source.root);
            if (rel.empty() || (*rel.begin() != "maps" && *rel.begin() != "clientscripts"))
                continue;
            const auto leaf = path.filename().wstring();
            if (leaf.size() < prefix.size() + 4 || !StartsWithName(leaf, prefix) ||
                (_wcsicmp(leaf.c_str() + leaf.size() - 4, L".gsc") != 0 &&
                 _wcsicmp(leaf.c_str() + leaf.size() - 4, L".csc") != 0))
                continue;
            auto name = files::Utf8(path.filename());
            if (const auto pos = name.find(source.name); pos != name.npos)
                name.replace(pos, source.name.size(), project);
            const auto output = rel.parent_path() / files::FromUtf8(name);
            const auto destination = root / output;
            auto text = Replace(ReadScript(path), source.name, project);
            const bool sourceArt = rel.parent_path() == fs::path("maps/mp/createart") && path.extension() == ".gsc";
            if (fs::exists(destination) && (!sourceArt || ReadScript(destination) != text))
                continue;
            if (sourceArt)
                text = "// WaW createart is initialized by the imported map script.\nmain()\n{\n    level.tweakfile = 1;\n}\n";
            files::WriteText(destination, text);
            diagnostics.warnings.push_back("script " + PosixPath(output) + " generated from template " + PosixPath(rel));
        }
    }

    void StageRequiredScripts(const fs::path& root, std::string_view project, const std::optional<ScriptTemplate>& source,
                              StageDiagnostics& diagnostics)
    {
        for (const auto& [folder, extension] : ScriptSides)
            for (auto kind : ScriptKinds)
            {
                const auto suffix = std::string(kind) + std::string(extension);
                const auto name = std::string(project) + suffix;
                const auto relative = files::FromUtf8(folder) / files::FromUtf8(name);
                const auto destination = root / relative;
                if (fs::exists(destination))
                    continue;
                fs::path candidate;
                if (source)
                    candidate = source->root / files::FromUtf8(folder) / files::FromUtf8(source->name + suffix);
                if (suffix == "_amb.gsc" && !fs::is_regular_file(candidate))
                {
                    files::WriteText(destination, "// Server ambience runs in the imported WaW scripts.\nmain()\n{\n}\n");
                    diagnostics.warnings.push_back("script " + name + " generated: WaW owns server ambience");
                }
                else if (!source)
                    diagnostics.errors.push_back("script " + PosixPath(relative) + " missing");
                else if (!fs::exists(candidate))
                    diagnostics.errors.push_back("script template " + WindowsPath(candidate) + " missing");
                else
                {
                    files::WriteText(destination, Replace(ReadScript(candidate), source->name, project));
                    diagnostics.warnings.push_back("script " + name + " generated from template " + files::Utf8(candidate.filename()));
                }
            }
    }

    void StageScriptRawfiles(const fs::path& root, const std::optional<fs::path>& bo2, StageDiagnostics& diagnostics)
    {
        const fs::path relative = "animtrees/fxanim_props.atr";
        const auto destination = root / relative;
        if (fs::exists(destination))
            return;
        if (!bo2)
        {
            diagnostics.errors.push_back("animtrees/fxanim_props.atr missing and --bo2 not given");
            return;
        }
        const auto source = *bo2 / "raw" / relative;
        if (!fs::exists(source))
        {
            diagnostics.errors.push_back(WindowsPath(source) + " missing");
            return;
        }
        fs::create_directories(destination.parent_path());
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing);
    }

    std::vector<std::string> MapScripts(const fs::path& root, std::string_view project)
    {
        std::vector<std::string> scripts;
        for (const auto& [folder, extension] : ScriptSides)
            for (auto sub : ScriptSubfolders)
                AppendScripts(scripts, root, files::FromUtf8(folder) / files::FromUtf8(sub), project, extension, true);
        AppendScripts(scripts, root, "maps/mp/animscripts/traverse", "waw2bo2_", ".gsc", true);
        AppendScripts(scripts, root, "aitype", "waw_", ".gsc", false);
        AppendScripts(scripts, root, "aitype/clientscripts", "waw_", ".csc", false);
        auto owned = Bo2Scripts(root);
        scripts.insert(scripts.end(), std::make_move_iterator(owned.begin()), std::make_move_iterator(owned.end()));
        for (const auto& file : files::GlobSuffix(root / "clientscripts/mp/waw", ".csc"))
            scripts.push_back(PosixPath(file.lexically_relative(root)));
        return scripts;
    }

    void RewriteTemplateReferences(const fs::path& root, std::string_view project, StageDiagnostics& diagnostics)
    {
        std::string bytes;
        for (const auto& script : MapScripts(root, project))
        {
            const auto path = root / files::FromUtf8(script);
            if (!files::ReadBytes(path, bytes))
                throw std::runtime_error("cannot read " + WindowsPath(path));
            if (bytes.find("zm_test") == bytes.npos)
                continue;
            const auto text = encoding::UniversalNewlines(encoding::Utf8Replace(bytes));
            files::WriteText(path, Replace(text, "zm_test", project));
            diagnostics.warnings.push_back("template references renamed in " + script);
        }
    }

    Json StageScriptFilesRequest(const fs::path& path)
    {
        const auto request = Json::parse(files::ReadText(path));
        const auto root = files::FromUtf8(request.at("root").get<std::string>());
        const auto& project = request.at("project").get_ref<const std::string&>();
        StageDiagnostics diagnostics;
        if (const auto source = GetTemplate(request.value("bo2Template", Json{})); source)
            StageTemplateScripts(root, project, *source, diagnostics);
        if (const auto required = request.find("requiredTemplate"); required != request.end())
            StageRequiredScripts(root, project, GetTemplate(*required), diagnostics);
        if (request.value("rewriteTemplateReferences", false))
            RewriteTemplateReferences(root, project, diagnostics);
        if (const auto bo2 = request.find("rawfilesBo2"); bo2 != request.end())
            StageScriptRawfiles(root, bo2->is_null() ? std::nullopt : std::optional{files::FromUtf8(bo2->get<std::string>())},
                                diagnostics);
        return {{"errors", std::move(diagnostics.errors)}, {"warnings", std::move(diagnostics.warnings)}};
    }

    Json MapScriptsRequest(const fs::path& path)
    {
        const auto request = Json::parse(files::ReadText(path));
        return MapScripts(files::FromUtf8(request.at("root").get<std::string>()), request.at("project").get_ref<const std::string&>());
    }
}

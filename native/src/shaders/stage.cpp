#include "stage.h"

#include "core/files.h"
#include "core/hash.h"
#include "core/parallel.h"
#include "d3d.h"
#include "translate.h"

#include <optional>
#include <unordered_set>

namespace w2b::shaders
{
    namespace
    {
        namespace fs = std::filesystem;
        using util::Json;

        struct Source
        {
            std::string name;   // relative posix path, e.g. "ps_black.hlsl.cso"
            std::string path;   // str(Path) of the source file
        };

        // Path.with_suffix(suffix) on a relative posix name
        std::string WithSuffix(const std::string& name, const std::string& suffix)
        {
            const auto slash = name.rfind('/');
            const auto dot = name.rfind('.');
            if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == (slash == std::string::npos ? 0 : slash + 1))
                return name + suffix;
            return name.substr(0, dot) + suffix;
        }

        std::string Backslashes(std::string text)
        {
            for (auto& c : text)
                if (c == '/')
                    c = '\\';
            return text;
        }

        // _stage_shader: None for T6 donor DXBC, else the report row
        std::optional<Json> StageOne(const Source& source, const fs::path& destination)
        {
            std::string data;
            if (!files::ReadBytes(files::FromUtf8(source.path), data))
            {
                Json row = Json::object();
                row["name"] = source.name;
                row["source"] = source.path;
                row["status"] = "unsupported";
                row["error"] = "[Errno 2] No such file or directory: '" + source.path + "'";
                return row;
            }
            if (data.substr(0, 4) == "DXBC")
                return std::nullopt;
            try
            {
                const auto assembly = Disassemble(data);
                auto translation = Translate(assembly, nullptr);
                const auto compiled = CompileHlsl(translation.hlsl, translation.report["target_profile"].get<std::string>());
                const auto output = destination / files::FromUtf8(source.name);
                files::WriteText(destination / files::FromUtf8(WithSuffix(source.name, ".hlsl")), translation.hlsl);
                files::WriteBytes(destination / files::FromUtf8(WithSuffix(source.name, ".cso")), compiled);
                auto& report = translation.report;
                report["output_sha256"] = hash::Sha256Hex(compiled);
                report["source"] = source.path;
                report["source_sha256"] = hash::Sha256Hex(data);
                report["compiled"] = true;
                files::WriteText(destination / files::FromUtf8(WithSuffix(source.name, ".json")), util::JsonDumps(report, 2) + "\n");
                Json row = report;
                row["name"] = source.name;
                row["status"] = "translated_unbound";
                (void)output;
                return row;
            }
            catch (const ShaderError& error)
            {
                Json row = Json::object();
                row["name"] = source.name;
                row["source"] = source.path;
                row["status"] = "unsupported";
                row["error"] = error.what();
                return row;
            }
        }
    } // namespace

    Json Stage(const std::vector<std::string>& roots, const fs::path& destination)
    {
        std::vector<Source> sources;
        std::unordered_set<std::string> names; // dict.setdefault: the first root wins
        for (const auto& root : roots)
        {
            for (const char* sub : {"shader_bin", "content_source\\shader_bin"})
            {
                const std::string folderText = root + "\\" + sub;
                const fs::path folder = files::FromUtf8(folderText);
                for (const auto& path : files::GlobSuffix(folder, ".cso"))
                {
                    std::error_code error;
                    if (!fs::is_regular_file(path, error))
                        continue; // rglob also yields folders named *.cso; read_bytes would fail on them
                    const auto generic = path.lexically_relative(folder).generic_u8string();
                    const std::string relative(reinterpret_cast<const char*>(generic.data()), generic.size());
                    if (names.insert(relative).second)
                        sources.push_back({relative, folderText + "\\" + Backslashes(relative)});
                }
            }
        }

        std::vector<std::optional<Json>> rows(sources.size());
        parallel::For(sources.size(), [&](const size_t i) { rows[i] = StageOne(sources[i], destination); });

        Json shaders = Json::array();
        size_t translated = 0, unsupported = 0;
        for (auto& row : rows)
        {
            if (!row)
                continue;
            const auto status = (*row)["status"].get<std::string>();
            translated += status == "translated_unbound";
            unsupported += status == "unsupported";
            shaders.push_back(std::move(*row));
        }
        Json report = Json::object();
        report["status"] = shaders.empty() ? "no_source_bytecode" : "requires_t6_pass_bindings";
        report["translated"] = translated;
        report["unsupported"] = unsupported;
        report["shaders"] = std::move(shaders);
        fs::create_directories(destination);
        files::WriteText(destination / "stage.json", util::JsonDumps(report, 2) + "\n");
        return report;
    }
} // namespace w2b::shaders

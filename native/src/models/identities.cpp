#include "models/identities.h"

#include "core/files.h"
#include "core/parallel.h"
#include "gsc/port/util.h"
#include "models/surface_fixes.h"
#include "weapons/stage.h"

namespace w2b::models
{
    namespace
    {
        constexpr std::string_view Prefix = "waw_world_model/";

        Json CopyAliases(const Path& folder)
        {
            auto renamed = Json::object();
            for (const auto& path : files::RGlob(folder, ".json"))
            {
                auto name = weapons::PosixPath(path.lexically_relative(folder));
                if (name.ends_with(".json"))
                    name.resize(name.size() - 5);
                if (name.starts_with(Prefix) || name.starts_with("waw_xmodel/") || name.starts_with("waw_fx_model/"))
                    continue;
                const auto output = std::string(Prefix) + name;
                std::string data;
                if (!files::ReadBytes(path, data))
                    throw std::runtime_error("cannot read " + files::Utf8(path));
                files::WriteBytes(folder / files::FromUtf8(output + ".json"), data);
                renamed[name] = output;
            }
            return renamed;
        }

        void RewriteManifest(Json& data, bool placement, const ModelNames& renamed)
        {
            if (data.is_object())
                for (auto& [key, value] : data.items())
                {
                    if (value.is_string() && (gsc::port::In(key, {"model", "xModel", "skyboxmodel", "modelName"}) ||
                                              (key == "name" && placement)))
                    {
                        const auto match = renamed.find(value.get_ref<const std::string&>());
                        if (match != renamed.end())
                            value = match->second;
                    }
                    else
                        RewriteManifest(value, placement, renamed);
                }
            else if (data.is_array())
                for (auto& item : data)
                    RewriteManifest(item, placement, renamed);
        }

        void RewriteCollisionAndRender(const Path& folder, const ModelNames& renamed)
        {
            std::error_code error;
            for (files::fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            {
                const auto path = it->path();
                if (gsc::Lower(files::Utf8(path.extension())) != ".json")
                    continue;
                auto data = weapons::ReadAssetJson(path);
                RewriteManifest(data, path.filename() == "models.json" || path.filename() == "staticmodels.json", renamed);
                ReplaceStagedText(path, py::JsonDumpsDefault(data));
            }
        }

        std::string ModelTable(const ModelNames& renamed)
        {
            std::string script = "// Source model identities; engine assets keep their own names.\ntable()\n{\n    models = [];\n";
            for (const auto& [name, target] : renamed)
            {
                // Split the first character so a later rewrite cannot match a
                // complete old model name, including UTF-8 names.
                std::size_t split = name.empty() ? 0 : 1;
                while (split < name.size() && (static_cast<unsigned char>(name[split]) & 0xc0) == 0x80)
                    ++split;
                script += "    models[\"" + name.substr(0, split) + "\" + \"" + name.substr(split) + "\"] = \"" + target + "\";\n";
            }
            return script + "    return models;\n}\n";
        }
    }

    void RewriteStagedScripts(const Path& project, const std::function<std::string(std::string_view)>& transform)
    {
        std::vector<Path> scripts;
        for (const auto folder : {"maps", "clientscripts"})
            for (const auto extension : {".gsc", ".csc"})
                for (auto& path : files::RGlob(project / folder, extension))
                    if (path.extension() == extension)
                        scripts.push_back(std::move(path));
        parallel::For(scripts.size(), [&](std::size_t i)
        {
            const auto& path = scripts[i];
            const auto source = files::ReadText(path);
            const auto generated = transform(source);
            if (source != generated)
                ReplaceStagedText(path, generated);
        }, 4);
    }

    Json IsolateModelNames(const Path& project)
    {
        auto result = CopyAliases(project / "xmodel");
        if (result.empty())
            return result;
        const auto names = result.get<ModelNames>();
        RewriteCollisionAndRender(project / "BSP", names);
        RewriteStagedScripts(project, [&](std::string_view source) { return RewriteModelLiterals(source, names); });
        for (const auto& path : files::RGlob(project / "fx", ".efx"))
        {
            auto text = files::ReadText(path);
            for (const auto& [old, value] : result.items())
                text = gsc::port::Replace(std::move(text), '"' + old + '"', '"' + value.get_ref<const std::string&>() + '"');
            ReplaceStagedText(path, text);
        }
        ReplaceStagedText(project / "maps/mp/waw/_waw2bo2_models.gsc", ModelTable(names));
        return result;
    }

    Json IsolateModelNamesRequest(const Path& path)
    {
        const auto request = weapons::ReadAssetJson(path);
        return IsolateModelNames(files::FromUtf8(request.at("project").get_ref<const std::string&>()));
    }
}

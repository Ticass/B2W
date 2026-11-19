#include "paths/paths.h"

#include "core/encoding.h"
#include "core/files.h"
#include "gsc/tokens.h"

namespace w2b::paths
{
    namespace
    {
        constexpr std::string_view TraverseFolder = "maps/mp/animscripts/traverse/";
        constexpr std::string_view Shared = "waw2bo2_dog_shared.gsc";

        void RemoveOld(const Path& folder)
        {
            if (!std::filesystem::is_directory(folder))
                return;
            for (const auto& entry : std::filesystem::directory_iterator(folder))
            {
                const auto name = gsc::Lower(files::Utf8(entry.path().filename()));
                if (!name.starts_with(Prefix) || !name.ends_with(".gsc"))
                    continue;
                if (std::filesystem::is_directory(entry.symlink_status()))
                    throw Error("expected traversal script file: " + files::Utf8(entry.path()));
                std::filesystem::remove(entry.path());
            }
        }

        void WriteScripts(const Path& project, const Json& renamed, const DogTraversals& dogs, Json& summary)
        {
            const auto folder = project / files::FromUtf8(TraverseFolder);
            RemoveOld(folder);
            const auto& generated = summary.at("generated");
            auto& paths = summary.at("scripts");
            Names names;
            for (const auto& script : renamed)
                if (const auto& name = script.get_ref<const std::string&>(); name.starts_with(Prefix))
                    names.insert(name);
            for (const auto& script : names)
            {
                Alias alias;
                Json calls = Json::array();
                if (const auto entry = generated.find(script); entry != generated.end())
                {
                    if (!entry->at(0).is_null())
                        alias = entry->at(0).get<std::string>();
                    calls = entry->at(1);
                }
                else
                    alias = script.substr(Prefix.size());
                const auto file = files::FromUtf8(script + ".gsc");
                if (file.has_parent_path() || file.has_root_path())
                    throw Error("invalid traversal script name: " + script);
                files::WriteText(folder / file, TraverseScript(alias, calls));
                paths.push_back(std::string(TraverseFolder) + script + ".gsc");
            }
            if (!generated.empty())
            {
                files::WriteText(folder / files::FromUtf8(Shared), dogs.library);
                paths.push_back(std::string(TraverseFolder) + std::string(Shared));
            }
        }
    }

    Json ReadManifest(const Path& file)
    {
        std::string input;
        if (!files::ReadBytes(file, input))
            throw std::runtime_error("cannot read " + files::Utf8(file));
        return Json::parse(input);
    }

    std::string ReadSource(const Path& file)
    {
        std::string input;
        if (!files::ReadBytes(file, input))
            throw std::runtime_error("cannot read " + files::Utf8(file));
        return encoding::UniversalNewlines(encoding::Utf8Replace(input));
    }

    Json Stage(const Path& input, const Path& bsp, const Path& project, const std::optional<Path>& asd, const std::vector<Path>& roots)
    {
        const auto source = ReadManifest(input);
        auto dogs = roots.empty() ? DogTraversals{} : SourceDogs(roots, SourceScripts(source));
        std::optional<Names> substates;
        if (asd)
        {
            substates = TraverseSubstates(ReadSource(*asd));
            if (substates->empty())
                throw Error(files::Utf8(*asd) + ": no zm_traverse substates with _crawl twins");
        }
        auto output = Convert(source, substates, dogs.calls);
        files::WriteText(bsp / "paths.json", py::JsonDumps(output.at("manifest"), -1) + "\n");
        WriteScripts(project, output.at("renamed"), dogs, output.at("summary"));
        return {{"summary", std::move(output.at("summary"))}, {"renamed", std::move(output.at("renamed"))},
                {"dogs", {{"calls", std::move(dogs.calls)}, {"library", std::move(dogs.library)},
                         {"states", std::move(dogs.states)}, {"errors", std::move(dogs.errors)}}}};
    }
}

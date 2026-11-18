#include "pipeline/actor_trees.h"

#include "assets/resolver.h"
#include "core/files.h"
#include "extract/stockwaw.h"
#include "gsc/port/rewrites.h"
#include "pipeline/finish.h"
#include "pipeline/modutil.h"

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

namespace w2b::pipeline
{
    namespace
    {
        using py::Json;
        using files::FromUtf8;
        constexpr auto multiline = std::regex::ECMAScript | std::regex::multiline;

        std::string EscapePattern(std::string_view word)
        {
            std::string escaped;
            for (const auto c : word)
            {
                if (std::string_view(R"(\^$.|?*+()[]{})").find(c) != std::string_view::npos)
                    escaped += '\\';
                escaped += c;
            }
            return escaped;
        }

        void InsertBodyAnimations(std::string& tree, const std::vector<std::string>& animations, const std::string& kind, const std::string& message)
        {
            static const std::regex body(R"(^body\s*\n\{)", multiline);
            std::smatch match;
            if (!std::regex_search(tree, match, body))
                throw StepError(kind, message);
            std::string extra = "\n";
            for (const auto& animation : animations)
                extra += '\t' + animation + '\n';
            tree.insert(static_cast<std::size_t>(match.position() + match.length()), extra);
        }

        std::vector<std::string> AddDonorStates(const fs::path& raw, const Json& donors, std::string& states, const std::string& tree)
        {
            const auto known = NativeTreeAnimations(tree);
            std::set<std::string> used(known.begin(), known.end());
            std::vector<std::string> added;
            for (const auto& [donor, donorStates] : donors.items())
            {
                const auto text = files::ReadText(raw / "animstatedefs" / FromUtf8(donor + ".asd"));
                for (const auto& entry : donorStates)
                {
                    const auto& state = entry.get_ref<const std::string&>();
                    const auto header = "^" + EscapePattern(state) + R"(\s*:)";
                    if (std::regex_search(states, std::regex(header, multiline)))
                        continue;
                    const std::regex block(header + R"([^\n]*\n\{[\s\S]*?^\})", multiline);
                    std::smatch match;
                    if (!std::regex_search(text, match, block))
                        throw StepError("StageError", donor + ".asd has no state " + state);
                    const auto definition = match.str();
                    states.resize(py::RStrip(states, "\n").size());
                    states += "\n\n" + definition + '\n';
                    const auto lines = py::SplitLines(definition);
                    for (std::size_t line = 2; line + 1 < lines.size(); ++line)
                    {
                        std::istringstream words{std::string(lines[line])};
                        std::string label, animation, extra;
                        if (words >> label >> animation && !(words >> extra) && used.insert(animation).second)
                            added.push_back(std::move(animation));
                    }
                }
            }
            return added;
        }

        fs::path FindAnimation(const std::vector<fs::path>& roots, extract::StockWaw* stock, const std::string& name)
        {
            const auto relative = assets::SourcePath({"xanim", name});
            for (const auto& root : roots)
                if (auto found = assets::FindFile(root, relative, false); !found.empty())
                    return found;
            if (stock)
                if (const auto found = stock->RootFor("xanim", name))
                    return assets::FindFile(found->second, relative, false);
            return {};
        }

        void AddDogStates(const fs::path& project, const std::vector<fs::path>& roots, extract::StockWaw* stock, const Json& requested,
                          std::string& states, std::string& tree, std::vector<std::string>& added, Json& errors)
        {
            std::string lines;
            std::set<std::string> names;
            const auto sorted = py::SortKeys(requested);
            for (const auto& [state, value] : sorted.items())
            {
                const auto animation = value.get<std::string>();
                const auto source = FindAnimation(roots, stock, animation);
                if (source.empty())
                {
                    errors.push_back("UNSUPPORTED_XANIM " + animation + " (" + state + "): no WaW xanim; the source dog traversal cannot play it");
                    continue;
                }
                const auto translated = gsc::port::AnimationAssetName(animation);
                const auto target = project / "xanim" / FromUtf8(translated);
                fs::create_directories(target.parent_path());
                fs::copy_file(source, target, fs::copy_options::overwrite_existing);
                fs::last_write_time(target, fs::last_write_time(source));
                names.insert(translated);
                lines += '\t' + state + "\t\t\t" + translated + '\n';
            }
            static const std::regex traverse(R"(^zm_traverse\s*:[^\n]*\n\{([\s\S]*?)^\})", multiline);
            std::smatch match;
            if (std::regex_search(states, match, traverse))
                states.insert(static_cast<std::size_t>(match.position(1) + match.length(1)), lines);
            else
            {
                states.resize(py::RStrip(states, "\n").size());
                states += "\n\nzm_traverse : aliased restart notify traverse_anim\n{\n" + lines + "}\n";
            }
            InsertBodyAnimations(tree, {names.begin(), names.end()}, "TraversalError", "dog animtree has no body group");
            for (const auto& animation : names)
                if (std::ranges::find(added, animation) == added.end())
                    added.push_back(animation);
        }

        void UpdateActorScripts(const fs::path& project, const std::string& base, const std::string& name, const std::vector<std::string>& added)
        {
            const auto replace = [](std::string& text, const std::string& from, const std::string& to)
            {
                for (std::size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size())
                    text.replace(pos, from.size(), to);
            };
            for (const auto& path : files::RGlob(project / "aitype", "sc"))
            {
                const auto file = mod::Lower(files::Utf8(path.filename()));
                const auto extension = files::Utf8(path.extension());
                if (!mod::IsFile(path) || !file.starts_with("waw_") || extension.size() != 4)
                    continue;
                auto text = files::ReadText(path);
                if (text.find('"' + base) == std::string::npos)
                    continue;
                replace(text, '"' + base + '.', '"' + name + '.');
                replace(text, '"' + base + '"', '"' + name + '"');
                if (!added.empty() && extension == ".gsc")
                {
                    text += "\nwaw_reference_added_states()\n{\n";
                    for (const auto& animation : added)
                        text += "    dummy_anim_ref = %" + animation + ";\n";
                    text += "}\n";
                }
                files::WriteText(path, text);
            }
        }
    }

    std::vector<std::string> NativeTreeAnimations(std::string_view tree)
    {
        static const std::regex comment(R"(//[^\n]*)"), groups(R"(^\s*([A-Za-z0-9_]+)\s*\n\s*\{)", multiline);
        const auto clean = std::regex_replace(std::string(tree), comment, "");
        std::set<std::string> seen;
        for (auto match = std::sregex_iterator(clean.begin(), clean.end(), groups); match != std::sregex_iterator(); ++match)
            seen.insert((*match)[1].str());
        std::vector<std::string> animations;
        for (auto line : py::SplitLines(clean))
        {
            line = py::Strip(line);
            if (!line.empty() && std::ranges::all_of(line, py::IsWord) && seen.emplace(line).second)
                animations.emplace_back(line);
        }
        return animations;
    }

    Json StageOwnedActorTree(const Json& request)
    {
        const auto project = FromUtf8(request.at("project_root").get<std::string>());
        const auto raw = FromUtf8(request.at("bo2_root").get<std::string>()) / "raw";
        const auto& base = request.at("base").get_ref<const std::string&>();
        const auto& config = request.at("config");
        auto states = files::ReadText(raw / "animstatedefs" / FromUtf8(base + ".asd"));
        auto tree = files::ReadText(raw / "animtrees" / FromUtf8(base + ".atr"));
        auto added = AddDonorStates(raw, config.value("add_states", Json::object()), states, tree);
        if (!added.empty())
            InsertBodyAnimations(tree, added, "StageError", base + ".atr has no body group");

        std::optional<extract::StockWaw> stock;
        auto errors = Json::array();
        if (const auto dogStates = request.value("source_states", Json::object()); !dogStates.empty())
        {
            stock = extract::StockWaw::FromJson(request.value("stock_waw", Json()));
            std::vector<fs::path> roots;
            for (const auto& root : request.at("roots"))
                roots.push_back(FromUtf8(root.get<std::string>()));
            AddDogStates(project, roots, stock ? &*stock : nullptr, dogStates, states, tree, added, errors);
        }
        const auto& name = config.at("owned_tree").get_ref<const std::string&>();
        files::WriteText(project / "animtrees" / FromUtf8(name + ".atr"), tree);
        files::WriteText(project / "animstatedefs" / FromUtf8(name + ".asd"), states);
        UpdateActorScripts(project, base, name, added);
        return {{"name", name}, {"tree", tree}, {"errors", errors}, {"stock_prepared", stock ? stock->PreparedJson() : Json()}};
    }

    Json StageOwnedActorTreeRequest(const fs::path& request)
    {
        try
        {
            return StageOwnedActorTree(mod::Json(request));
        }
        catch (const StepError& error)
        {
            return {{"error_kind", error.kind}, {"error", error.what()}};
        }
    }
}

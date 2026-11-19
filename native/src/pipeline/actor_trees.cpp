#include "pipeline/actor_trees.h"

#include "assets/resolver.h"
#include "core/files.h"
#include "core/pyre.h"
#include "extract/stockwaw.h"
#include "gsc/port/rewrites.h"
#include "pipeline/finish.h"
#include "pipeline/modutil.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace w2b::pipeline
{
    namespace
    {
        using py::Json;
        using files::FromUtf8;
        // pyre reports code point offsets; std::string::insert wants bytes
        std::size_t ByteOffset(std::string_view text, long long codePoints)
        {
            long long seen = 0;
            for (std::size_t byte = 0; byte < text.size(); ++byte)
                if ((static_cast<unsigned char>(text[byte]) & 0xC0) != 0x80 && seen++ == codePoints)
                    return byte;
            return text.size();
        }

        void InsertBodyAnimations(std::string& tree, const std::vector<std::string>& animations, const std::string& kind, const std::string& message)
        {
            static const pyre::Regex body(R"(^body\s*\n\{)", pyre::MULTILINE);
            const auto match = body.Search(tree);
            if (!match)
                throw StepError(kind, message);
            std::string extra = "\n";
            for (const auto& animation : animations)
                extra += '\t' + animation + '\n';
            tree.insert(ByteOffset(tree, match->End()), extra);
        }

        std::vector<std::string> AddDonorStates(const fs::path& raw, const std::vector<std::pair<std::string, std::vector<std::string>>>& donors, std::string& states, const std::string& tree)
        {
            const auto known = NativeTreeAnimations(tree);
            std::set<std::string> used(known.begin(), known.end());
            std::vector<std::string> added;
            for (const auto& [donor, donorStates] : donors)
            {
                const auto text = files::ReadText(raw / "animstatedefs" / FromUtf8(donor + ".asd"));
                for (const auto& state : donorStates)
                {
                    const auto header = "^" + pyre::Escape(state) + R"(\s*:)";
                    if (pyre::Regex(header, pyre::MULTILINE).Search(states))
                        continue;
                    const auto match = pyre::Regex(header + R"([^\n]*\n\{[\s\S]*?^\})", pyre::MULTILINE).Search(text);
                    if (!match)
                        throw StepError("StageError", donor + ".asd has no state " + state);
                    const auto definition = (*match)[0];
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

        void AddDogStates(const fs::path& project, const std::vector<fs::path>& roots, extract::StockWaw* stock,
                          const std::map<std::string, std::string>& requested, std::string& states, std::string& tree,
                          std::vector<std::string>& added, std::vector<std::string>& errors)
        {
            std::string lines;
            std::set<std::string> names;
            for (const auto& [state, animation] : requested)
            {
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
            static const pyre::Regex traverse(R"(^zm_traverse\s*:[^\n]*\n\{([\s\S]*?)^\})", pyre::MULTILINE);
            if (const auto match = traverse.Search(states))
                states.insert(ByteOffset(states, match->End(1)), lines);
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
        static const pyre::Regex comment(R"(//[^\n]*)"), groups(R"(^\s*([A-Za-z0-9_]+)\s*\n\s*\{)", pyre::MULTILINE);
        const auto clean = comment.Sub("", tree);
        std::set<std::string> seen;
        for (const auto& match : groups.FindIter(clean))
            seen.insert(match[1]);
        std::vector<std::string> animations;
        for (auto line : py::SplitLines(clean))
        {
            line = py::Strip(line);
            if (!line.empty() && std::ranges::all_of(line, py::IsWord) && seen.emplace(line).second)
                animations.emplace_back(line);
        }
        return animations;
    }

    OwnedActorTree StageOwnedActorTree(const OwnedActorTreeRequest& request)
    {
        const auto raw = request.bo2Root / "raw";
        auto states = files::ReadText(raw / "animstatedefs" / FromUtf8(request.base + ".asd"));
        auto tree = files::ReadText(raw / "animtrees" / FromUtf8(request.base + ".atr"));
        auto added = AddDonorStates(raw, request.donorStates, states, tree);
        if (!added.empty())
            InsertBodyAnimations(tree, added, "StageError", request.base + ".atr has no body group");
        std::vector<std::string> errors;
        if (!request.sourceStates.empty())
            AddDogStates(request.project, request.roots, request.stock, request.sourceStates, states, tree, added, errors);
        const auto& name = request.ownedTree;
        files::WriteText(request.project / "animtrees" / FromUtf8(name + ".atr"), tree);
        files::WriteText(request.project / "animstatedefs" / FromUtf8(name + ".asd"), states);
        UpdateActorScripts(request.project, request.base, name, added);
        return {name, std::move(tree), std::move(errors)};
    }
}

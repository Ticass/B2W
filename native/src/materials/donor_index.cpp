#include "materials/donor_index.h"

#include "core/files.h"
#include "core/parallel.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <tuple>

namespace w2b::materials
{
    namespace
    {
        using files::fs::path;

        bool Animated(std::string name)
        {
            // The reference pattern contains a literal backspace after "_pan".
            constexpr std::array<std::string_view, 11> markers{"moving", "scroll", "flow", "pulse", "spin",
                "rotat", "flicker", "_pan\b", "uvanim", "water", "lava"};
            for (char& c : name)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
            return std::ranges::any_of(markers, [&](auto marker) { return name.find(marker) != name.npos; });
        }

        struct Candidate
        {
            std::string technique;
            std::string filename;
            std::size_t textures;
            int directoryRank;
            bool animated;

            auto Rank() const { return std::tie(textures, directoryRank, filename); }
        };

        std::optional<Candidate> ReadCandidate(const path& file, const path& stock)
        {
            py::Json data;
            try
            {
                const auto text = files::ReadText(file);
                if (text.starts_with("\xef\xbb\xbf"))
                    return std::nullopt;
                data = py::Json::parse(text);
            }
            catch (const std::runtime_error&)
            {
                return std::nullopt;
            }
            catch (const py::Json::parse_error&)
            {
                return std::nullopt;
            }
            const auto technique = data.value("techniqueSet", py::Json{});
            if (!technique.is_string() || technique.get_ref<const std::string&>().empty())
                return std::nullopt;
            const auto& name = technique.get_ref<const std::string&>();
            const auto relative = file.lexically_relative(stock);
            const auto& directory = *relative.begin();
            const int rank = directory == "wpc" ? 0 : directory == "generated" ? 1 : 9;
            const auto textures = data.find("textures");
            return Candidate{name, files::Utf8(file), textures == data.end() ? 0 : textures->size(),
                             rank, Animated(files::Utf8(file.stem())) || Animated(name)};
        }
    }

    py::Json IndexDonors(const files::fs::path& stockMaterials)
    {
        const auto paths = files::RGlob(stockMaterials, ".json");
        std::vector<std::optional<Candidate>> candidates(paths.size());
        parallel::For(paths.size(), [&](std::size_t index)
        {
            candidates[index] = ReadCandidate(paths[index], stockMaterials);
        });
        struct Choice
        {
            const Candidate* best;
            bool hasStatic;
        };
        std::map<std::string_view, Choice> choices;
        for (const auto& candidate : candidates)
        {
            if (!candidate)
                continue;
            auto& [best, hasStatic] = choices.try_emplace(candidate->technique, &*candidate, false).first->second;
            if (candidate->Rank() < best->Rank())
                best = &*candidate;
            hasStatic |= !candidate->animated;
        }
        auto donors = py::Json::object();
        for (const auto& [technique, choice] : choices)
            if (choice.hasStatic)
                donors[technique] = choice.best->filename;
        return donors;
    }
}

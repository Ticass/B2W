#include "materials/techsets.h"

#include <algorithm>
#include <map>
#include <optional>
#include <tuple>

namespace w2b::materials
{
    namespace
    {
        bool In(char value, std::string_view group) { return group.find(value) != group.npos; }

        std::optional<int> LayerCost(const Layer& source, const Layer& target,
                                     std::vector<std::string>& notes, bool sourcePrograms)
        {
            const auto prefix = "layer " + std::to_string(source.index) + ": ";
            int cost = 0;
            if (source.blend != target.blend)
            {
                if (sourcePrograms && source.index > 0 && In(source.blend, "bm") && In(target.blend, "bm"))
                {
                    ++cost;
                    notes.push_back(prefix + "source program supplies '" + source.blend + "' blend");
                }
                else if (In(source.blend, "tba") && In(target.blend, "tba"))
                {
                    cost += 6;
                    notes.push_back(prefix + "blend '" + source.blend + "' -> '" + target.blend + "'");
                }
                else
                    return std::nullopt;
            }
            for (const auto feature : source.features)
            {
                if (target.features.contains(feature))
                    continue;
                switch (feature)
                {
                case 'c': return std::nullopt;
                case 'd': cost += 3; notes.push_back(prefix + "detail map dropped"); break;
                case 'n': cost += 12; notes.push_back(prefix + "normal map dropped"); break;
                case 's': cost += 10; notes.push_back(prefix + "specular map dropped"); break;
                default: cost += 5; notes.push_back(prefix + "'" + feature + "' dropped"); break;
                }
            }
            for (const auto feature : target.features)
            {
                if (source.features.contains(feature))
                    continue;
                switch (feature)
                {
                case 'n': cost += 1; break;
                case 'x': cost += 2; break;
                case 'v': cost += 4; break;
                default: return std::nullopt;
                }
            }
            return cost;
        }

        using Rejections = std::set<std::string>;

        std::optional<Match> MatchLit(const Technique& source, std::span<const Technique> candidates,
                                      const Rejections& rejected, Family family, bool sourcePrograms = false)
        {
            std::optional<Match> best;
            for (const auto& candidate : candidates)
            {
                if (!candidate.lit || candidate.family != family || !candidate.modifiers.empty() ||
                    candidate.layers.size() != source.layers.size() || rejected.contains(candidate.name))
                    continue;
                Match match{candidate.name, static_cast<int>(source.modifiers.size()) * 2, {}};
                for (const auto& modifier : source.modifiers)
                    match.notes.push_back("modifier '" + modifier + "' dropped");
                bool accepted = true;
                for (std::size_t i = 0; i < source.layers.size(); ++i)
                {
                    const auto delta = LayerCost(source.layers[i], candidate.layers[i], match.notes, sourcePrograms);
                    if (!delta)
                    {
                        accepted = false;
                        break;
                    }
                    match.cost += *delta;
                }
                if (accepted && (!best || std::tie(match.cost, match.target) < std::tie(best->cost, best->target)))
                    best = std::move(match);
            }
            return best;
        }

        template <typename Accept>
        const Technique* FirstCandidate(std::span<const Technique> candidates, const Rejections& rejected, Accept accept)
        {
            const Technique* first = nullptr;
            for (const auto& candidate : candidates)
                if ((!first || candidate.name < first->name) && !rejected.contains(candidate.name) && accept(candidate))
                    first = &candidate;
            return first;
        }

        bool Unlit(const Technique& candidate, Family family, std::string_view kind)
        {
            return !candidate.lit && candidate.family == family && candidate.unlit == kind;
        }

        Match WithNote(Match match, std::string note, int cost = 0)
        {
            match.notes.insert(match.notes.begin(), std::move(note));
            match.cost += cost;
            return match;
        }

        std::string_view StripHash(std::string_view name)
        {
            if (name.size() > 8 && name[name.size() - 9] == '_' &&
                std::ranges::all_of(name.substr(name.size() - 8), [](char c)
                    { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }))
                name.remove_suffix(9);
            return name;
        }

        Match MatchUnlit(const Technique& source, std::span<const Technique> candidates,
                         const Rejections& rejected, bool layeredVertices)
        {
            const auto quoted = "'" + source.name + "'";
            if (source.family == Family::other && source.unlit == "2d")
            {
                const auto target = FirstCandidate(candidates, rejected, [](const Technique& candidate)
                    { return Unlit(candidate, Family::other, "trivial"); });
                if (!target)
                    throw TechniqueError("no stock T6 HUD trivial technique set available for " + quoted);
                return {target->name, 0, {"T4 2d -> T6 trivial HUD pass (same-asset reticle measurement)"}};
            }
            if (source.family == Family::model)
            {
                static const std::map<std::string_view, std::string_view> native{
                    {"treecanopy", "treecanopy"}, {"unlit", "unlit_replace"}, {"unlit_blend", "unlit_blend"},
                    {"unlit_add", "unlit_add"}, {"objective", "objective"}};
                if (const auto kind = native.find(source.unlit); kind != native.end())
                    if (const auto target = FirstCandidate(candidates, rejected, [&](const Technique& candidate)
                        { return Unlit(candidate, Family::model, kind->second); }))
                        return {target->name, 0, {}};
                static const std::map<std::string_view, std::pair<std::string_view, std::string_view>> fallback{
                    {"unlit", {"l_sm_r0c0", "unlit model technique lit by the light grid"}},
                    {"sky_noncubemap", {"l_sm_r0c0", "non-cubemap sky model lit by the light grid"}},
                    {"objective", {"l_sm_r0c0", "objective technique lit by the light grid"}},
                    {"cooktorrance_sm", {"l_sm_r0c0n0s0", "cook-torrance approximated by lit_sm"}},
                    {"projected_decal_unlit", {"l_sm_t0c0", "projected decal drawn as alpha-tested model"}},
                    {"ambient_t0c0_sco", {"l_sm_t0c0", "ambient-only technique lit by the light grid"}}};
                const auto match = fallback.find(source.unlit);
                if (match == fallback.end())
                    throw TechniqueError("no T6 mapping rule for model technique set " + quoted);
                const auto& [replacement, note] = match->second;
                return WithNote(MatchTechnique("mc_" + std::string(replacement), candidates, rejected, false,
                                               layeredVertices), std::string(note), 20);
            }

            static const std::map<std::string_view, std::pair<std::string_view, std::string_view>> rules{
                {"unlit_blend", {"unlit_blend", ""}}, {"unlit", {"unlit_replace", ""}},
                {"unlit_distfalloff", {"unlit_blend", "distance falloff dropped"}},
                {"unlit_falloff_add", {"unlit_blend", ""}},
                {"tools", {"shadowcaster", "tools technique mapped to shadowcaster"}},
                {"shadowcaster", {"shadowcaster", ""}}};
            const auto rule = rules.find(source.unlit);
            if (rule == rules.end())
            {
                if (const auto same = FirstCandidate(candidates, rejected, [&](const Technique& candidate)
                    { return StripHash(candidate.name) == source.name; }))
                    return {same->name, 0, {"same-named T6 technique set"}};
                throw TechniqueError("no T6 mapping rule for unlit technique set " + quoted);
            }
            const auto& [kind, note] = rule->second;
            const auto target = FirstCandidate(candidates, rejected, [&](const Technique& candidate)
                { return Unlit(candidate, Family::world, kind); });
            if (!target)
                throw TechniqueError("no stock T6 '" + std::string(kind) + "' technique set available for " + quoted);
            Match match{target->name, 0, {}};
            return note.empty() ? match : WithNote(std::move(match), std::string(note), 5);
        }

        Match Trimmed(Match match, const Technique& source, std::size_t keep, std::string_view reason)
        {
            std::vector<std::string> notes;
            for (std::size_t i = keep; i < source.layers.size(); ++i)
                notes.push_back("layer " + std::to_string(source.layers[i].index) + " dropped (" + std::string(reason) + ")");
            match.cost += 15 * static_cast<int>(notes.size());
            match.notes.insert(match.notes.begin(), notes.begin(), notes.end());
            return match;
        }
    }

    Match MatchTechnique(std::string_view name, std::span<const Technique> candidates,
                         const std::set<std::string>& rejected, bool sourcePrograms, bool layeredVertices)
    {
        if (name == "m_rope")
            return WithNote(MatchTechnique("mc_l_sm_r0c0", candidates, rejected, false, layeredVertices),
                            "WaW rope -> T6 model-lit rope stream (same-asset measurement)");
        if (name == "default")
            return WithNote(MatchTechnique("mc_unlit", candidates, rejected, false, layeredVertices),
                            "WaW default colorMap -> T6 model unlit pass");
        const auto source = ParseTechnique(name);
        if (!source.lit)
            return MatchUnlit(source, candidates, rejected, layeredVertices);

        auto truncated = source;
        if (source.family == Family::layered && !layeredVertices)
        {
            if (truncated.layers.size() > 1)
                truncated.layers.resize(1);
            if (auto best = MatchLit(truncated, candidates, rejected, Family::world))
                return Trimmed(std::move(*best), source, truncated.layers.size(), "vertex layer data not converted");
            throw TechniqueError("no single-layer T6 world technique set for the base layer of '" + source.name + "'");
        }
        const auto family = source.family == Family::modelVertex ? Family::model : source.family;
        if (auto best = MatchLit(source, candidates, rejected, family, sourcePrograms))
            return *best;
        while (truncated.layers.size() > 1)
        {
            truncated.layers.pop_back();
            if (auto best = MatchLit(truncated, candidates, rejected, family))
                return Trimmed(std::move(*best), source, truncated.layers.size(),
                               "no " + std::to_string(source.layers.size()) + "-layer T6 technique");
        }
        throw TechniqueError("no compatible stock T6 technique set for '" + source.name + "'");
    }
}

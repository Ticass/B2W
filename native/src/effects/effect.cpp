#include "effects/effect.h"

#include "core/unicode.h"

#include <algorithm>
#include <array>
#include <format>
#include <numeric>

namespace w2b::effects
{
    namespace
    {
        using util::Json;
        using Names = std::set<std::string>;

        constexpr std::array<std::string_view, 3> linkedEffects{"effectOnImpact", "effectOnDeath", "effectEmitted"};
        constexpr std::array<unsigned, 13> typeFlags{0, 0, 0, 0, 0, 0x100, 0, 0x20, 0x10, 0, 0x40, 0, 0x80};

        void AddString(Names& set, const Json& value)
        {
            if (value.is_string() && !value.get_ref<const std::string&>().empty())
                set.insert(value.get_ref<const std::string&>());
        }

        void AddField(Names& set, const Json& object, std::string_view name)
        {
            if (const auto entry = object.find(name); entry != object.end())
                AddString(set, *entry);
        }

        long long RangeSum(const Json& range)
        {
            return std::accumulate(range.begin(), range.end(), 0ll, [](long long sum, const Json& value)
            {
                return sum + value.get<long long>();
            });
        }

        long long NonLoopingLife(const Json& effect)
        {
            const auto& elements = effect.at("elemDefs");
            const auto first = effect.at("elemDefCountLooping").get<std::size_t>();
            const auto count = effect.at("elemDefCountOneShot").get<std::size_t>() +
                               effect.at("elemDefCountEmission").get<std::size_t>();
            const auto end = std::min(elements.size(), first + count);
            long long lifetime = 0;
            for (auto i = first; i < end; ++i)
            {
                const auto& element = elements[i];
                lifetime = std::max(lifetime, RangeSum(element.at("spawnDelayMsec")) + RangeSum(element.at("lifeSpanMsec")));
            }
            return lifetime;
        }
    }

    Dependencies FindDependencies(const util::Json& source)
    {
        Dependencies deps;
        for (const auto& element : source.at("elemDefs"))
        {
            for (const auto name : linkedEffects)
                AddField(deps.effects, element, name);
            const bool isTrail = element.at("elemType") == sourceTrail;
            for (const auto& visual : element.at("visuals"))
            {
                AddField(deps.effects, visual, "effect");
                AddField(deps.materials, visual, "material");
                AddField(deps.models, visual, "model");
                AddField(deps.sounds, visual, "sound");
                if (isTrail)
                    AddField(deps.trailMaterials, visual, "material");
                if (const auto materials = visual.find("materials"); materials != visual.end())
                    for (const auto& material : *materials)
                        AddString(deps.materials, material);
            }
        }
        return deps;
    }

    std::size_t AllocationSize(const util::Json& effect)
    {
        std::size_t size = 76;
        Names strings{effect.at("name").get<std::string>()};
        for (const auto& element : effect.at("elemDefs"))
        {
            size += 292 + 96 * element.at("velSamples").size() + 48 * element.at("visSamples").size();
            const auto visuals = element.at("visualCount").get<unsigned>();
            if (element.at("elemType") == decal)
                size += 8 * visuals;
            else if (visuals > 1)
                size += 4 * visuals;
            if (const auto shape = element.find("trail"); shape != element.end() && !shape->empty())
                size += 28 + 20 * shape->at("verts").size() + 2 * shape->at("inds").size();
            if (const auto light = element.find("spotLight"); light != element.end() && !light->empty())
                size += 12;
            for (const auto name : linkedEffects)
                AddField(strings, element, name);
            AddField(strings, element, "effectAttached");
            AddField(strings, element, "spawnSound");
            for (const auto& visual : element.at("visuals"))
            {
                AddField(strings, visual, "effect");
                AddField(strings, visual, "sound");
            }
        }
        return std::accumulate(strings.begin(), strings.end(), size, [](std::size_t total, const std::string& name)
        {
            return total + unicode::Decode(name).size() + 1;
        });
    }

    ConvertedEffect ConvertEffect(const util::Json& source, const MaterialNames& materials)
    {
        const auto name = source.value("name", Json{});
        if (source.value("_type", Json{}) != "waw2bo2_fx" || source.value("_game", Json{}) != "T4")
            throw EffectError((name.is_string() ? name.get<std::string>() : "None") + ": not a T4 waw2bo2_fx dump");
        const auto& sourceName = name.get_ref<const std::string&>();
        ConvertedEffect effect;
        auto elements = Json::array();
        const auto flags = source.at("flags").get<unsigned>();
        auto convertedFlags = flags & 1u;
        for (const auto& element : source.at("elemDefs"))
        {
            auto converted = ConvertElement(element, materials, effect.notes, std::format("{} elem {}", sourceName, elements.size()));
            convertedFlags |= typeFlags.at(converted.at("elemType").get<std::size_t>());
            if (converted.at("lightingFrac").get<double>() > 0)
                convertedFlags |= 1;
            elements.push_back(std::move(converted));
        }
        if (const auto unknown = flags & ~1u)
            effect.notes.push_back(std::format("{}: unknown T4 effect flag bits 0x{:x} dropped", sourceName, unknown));
        auto& out = effect.asset;
        out = {{"_type", "waw2bo2_fx"}, {"_game", "T6"}, {"_version", 1}, {"_source", {{"game", "T4"}, {"name", name}}},
               {"name", std::string(outputPrefix) + sourceName}, {"flags", convertedFlags},
               {"efPriority", source.at("efPriority")}, {"elemDefCountLooping", source.at("elemDefCountLooping")},
               {"elemDefCountOneShot", source.at("elemDefCountOneShot")}, {"elemDefCountEmission", source.at("elemDefCountEmission")},
               {"totalSize", 0}, {"msecLoopingLife", source.at("msecLoopingLife")}, {"msecNonLoopingLife", 0},
               {"boundingBoxDim", {0.0, 0.0, 0.0}}, {"boundingBoxCentre", {0.0, 0.0, 0.0}},
               {"occlusionQueryDepthBias", 0.0}, {"occlusionQueryFadeIn", 0}, {"occlusionQueryFadeOut", 0},
               {"occlusionQueryScaleRange", {0.0, 0.0}}, {"elemDefs", std::move(elements)}};
        out["msecNonLoopingLife"] = NonLoopingLife(out);
        out["totalSize"] = AllocationSize(out);
        return effect;
    }
}

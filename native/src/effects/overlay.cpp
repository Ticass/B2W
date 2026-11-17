#include "effects/stage.h"

#include "core/files.h"
#include "effects/effect.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace w2b::effects
{
    namespace
    {
        using py::Json;

        bool Zero(const Json& value)
        {
            if (value.is_array() || value.is_object())
                return std::ranges::all_of(value, Zero);
            return std::abs(value.get<double>()) < 1e-6;
        }

        bool NoVisualAnimation(const Json& sample)
        {
            const auto& base = sample.at("base");
            const auto& amplitude = sample.at("amplitude");
            return base.at("scale") == 1 && amplitude.at("scale") == 0 &&
                   Zero(base.at("rotationDelta")) && Zero(base.at("rotationTotal")) &&
                   Zero(amplitude.at("rotationDelta")) && Zero(amplitude.at("rotationTotal"));
        }

        std::string StationaryModel(const Json& element)
        {
            if (element.at("elemType") != model || element.at("visualCount") != 1 ||
                element.at("spawnOneShot") != Json{1, 0} || !Zero(element.at("spawnDelayMsec")) ||
                (element.at("flags").get<unsigned>() & (0x200 | 0x400 | 0x08000000)))
                return {};
            if (!Zero(element.at("initialRotation")) ||
                std::ranges::any_of(element.at("spawnAngles"), [](const Json& range)
                {
                    return !Zero(range.at(1));
                }))
                return {};
            for (const std::string_view field : {"spawnOrigin", "spawnOffsetRadius", "spawnOffsetHeight",
                    "angularVelocity", "gravity", "velSamples", "windInfluence"})
                if (!Zero(element.at(field)))
                    return {};
            for (const std::string_view field : {"effectOnImpact", "effectOnDeath", "effectEmitted",
                    "effectAttached", "spawnSound"})
                if (const auto value = element.find(field); value != element.end() && value->is_string() && *value != "")
                    return {};
            if (!std::ranges::all_of(element.at("visSamples"), NoVisualAnimation))
                return {};
            const auto name = element.at("visuals").at(0).value("model", std::string{});
            return name.starts_with(modelPrefix) ? name.substr(modelPrefix.size()) : std::string{};
        }

        Json StageVariants(const files::fs::path& root, const std::string& source, Json effect)
        {
            const auto first = effect.at("elemDefCountLooping").get<std::size_t>();
            const auto end = first + effect.at("elemDefCountOneShot").get<std::size_t>();
            const auto& elements = effect.at("elemDefs");
            std::map<std::string, std::vector<std::size_t>> models;
            for (auto i = first; i < end; ++i)
                if (auto name = StationaryModel(elements.at(i)); !name.empty())
                    models[std::move(name)].push_back(i);

            auto entries = Json::array();
            for (const auto& [name, removed] : models)
            {
                auto variant = effect;
                const auto variantName = effect.at("name").get<std::string>() + "/reuse_model_" + std::to_string(entries.size());
                variant["name"] = variantName;
                auto kept = Json::array();
                for (std::size_t i = 0; i < elements.size(); ++i)
                    if (!std::ranges::binary_search(removed, i))
                        kept.push_back(elements[i]);
                variant["elemDefCountOneShot"] = end - first - removed.size();
                if (std::ranges::none_of(kept, [](const Json& e) { return e.at("elemType") == model; }))
                    variant["flags"] = effect.at("flags").get<unsigned>() & ~0x20u;
                variant["elemDefs"] = std::move(kept);
                variant["totalSize"] = AllocationSize(variant);
                files::WriteText(root / files::FromUtf8(variantName + ".w2bfx.json"), py::JsonDumps(variant, 1) + "\n");
                entries.push_back({{"source", source}, {"model", name}, {"variant", variantName}});
            }
            return entries;
        }
    }

    py::Json StageOverlayEffects(const files::fs::path& requestPath)
    {
        const auto request = Json::parse(files::ReadText(requestPath));
        const auto root = files::FromUtf8(request.at("project").get<std::string>()) / "fx";
        const auto table = request.at("effects").get<std::map<std::string, std::string>>();
        auto variants = Json::array();
        for (const auto& [source, name] : table)
        {
            const auto path = root / files::FromUtf8(name + ".w2bfx.json");
            if (!files::fs::exists(path))
                continue;
            auto entries = StageVariants(root, source, Json::parse(files::ReadText(path)));
            for (auto& entry : entries)
                variants.push_back(std::move(entry));
        }
        return variants;
    }
}

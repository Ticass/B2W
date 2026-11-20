#include "effects/stage.h"

#include "core/files.h"
#include "core/parallel.h"
#include "effects/effect.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace w2b::effects
{
    namespace
    {
        using util::Json;

        struct Overlay
        {
            ModelOverlay entry;
            std::string bytes;
        };

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

        std::vector<Overlay> StageVariants(const std::string& source, const Json& effect)
        {
            const auto first = effect.at("elemDefCountLooping").get<std::size_t>();
            const auto end = first + effect.at("elemDefCountOneShot").get<std::size_t>();
            const auto& elements = effect.at("elemDefs");
            std::map<std::string, std::vector<std::size_t>> models;
            for (auto i = first; i < end; ++i)
                if (auto name = StationaryModel(elements.at(i)); !name.empty())
                    models[std::move(name)].push_back(i);

            std::vector<Overlay> entries;
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
                entries.push_back({{source, name, variantName}, util::JsonDumps(variant, 1) + "\n"});
            }
            return entries;
        }

        std::vector<ModelOverlay> StageOverlays(const files::fs::path& root, const std::map<std::string, std::string>& table)
        {
            const std::vector<std::pair<std::string, std::string>> inputs(table.begin(), table.end());
            std::vector<ModelOverlay> variants;
            const auto read = [&](const auto& input) -> std::vector<Overlay>
            {
                const auto& [source, name] = input;
                const auto path = root / files::FromUtf8(name + ".w2bfx.json");
                std::string bytes;
                if (!files::ReadBytes(path, bytes))
                {
                    if (!files::fs::exists(path))
                        return {};
                    throw std::runtime_error("cannot read " + files::Utf8(path));
                }
                return StageVariants(source, Json::parse(bytes));
            };
            const auto write = [&](std::vector<Overlay>&& entries)
            {
                for (auto& [entry, bytes] : entries)
                {
                    files::WriteText(root / files::FromUtf8(entry.variant + ".w2bfx.json"), bytes);
                    variants.push_back(std::move(entry));
                }
            };

            // A generated variant can also be an input. Preserve source order
            // in that case so the reader sees any variant already written.
            const bool mayReadOutput = std::ranges::any_of(inputs, [](const auto& input)
            {
                auto name = input.second;
                std::ranges::transform(name, name.begin(), [](unsigned char c)
                {
                    return static_cast<char>(std::tolower(c));
                });
                const auto leaf = std::string_view(name).substr(name.find_last_of("/\\") + 1);
                return leaf.starts_with("reuse_model_");
            });
            if (mayReadOutput)
            {
                for (const auto& input : inputs)
                    write(read(input));
            }
            else
            {
                std::vector<std::vector<Overlay>> prepared(inputs.size());
                parallel::For(inputs.size(), [&](std::size_t i) { prepared[i] = read(inputs[i]); }, 4);
                for (auto& entries : prepared)
                    write(std::move(entries));
            }
            return variants;
        }
    }

    util::Json ToJson(const std::vector<ModelOverlay>& overlays)
    {
        auto json = Json::array();
        for (const auto& overlay : overlays)
            json.push_back({{"source", overlay.source}, {"model", overlay.model}, {"variant", overlay.variant}});
        return json;
    }

    std::vector<ModelOverlay> StageModelOverlayRuntime(const files::fs::path& root, const std::map<std::string, std::string>& effects)
    {
        auto entries = StageOverlays(root / "fx", effects);
        const auto assets = root / "maps/mp/waw/_waw2bo2_assets.gsc";
        if (!files::fs::exists(assets))
            return entries;

        auto text = files::ReadText(assets);
        constexpr std::string_view beginMarker = "\n    // BEGIN MODEL OVERLAY FX";
        constexpr std::string_view endMarker = "    // END MODEL OVERLAY FX\n";
        std::size_t cursor = 0;
        while ((cursor = text.find(beginMarker, cursor)) != text.npos)
        {
            const auto end = text.find(endMarker, cursor + beginMarker.size());
            if (end == text.npos)
                break;
            text.erase(cursor, end + endMarker.size() - cursor);
        }
        constexpr std::string_view init = "init()\n{";
        if (const auto start = text.find(init); start != text.npos)
        {
            const std::string table = "    level.waw2bo2_model_overlay_fx";
            std::string block(beginMarker);
            block += "\n" + table + " = [];\n";
            std::set<std::string> sources;
            for (const auto& entry : entries)
                sources.insert(entry.source);
            for (const auto& source : sources)
                block += table + "[" + util::JsonDumpsDefault(source) + "] = [];\n";
            for (const auto& entry : entries)
                block += table + "[" + util::JsonDumpsDefault(entry.source) + "][" + util::JsonDumpsDefault(entry.model) + "] = " +
                         util::JsonDumpsDefault(entry.variant) + ";\n";
            block += endMarker;
            text.insert(start + init.size(), block);
        }
        files::WriteText(assets, text);
        return entries;
    }
}

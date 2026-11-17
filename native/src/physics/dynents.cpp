#include "physics/physics.h"

#include "core/files.h"
#include "weapons/stage.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace w2b::physics
{
    namespace
    {
        constexpr auto Clutter = 1;
        constexpr auto Destructible = 2;
        constexpr auto SpawnBody = 1;

        void Validate(const Json& entity, std::size_t index)
        {
            const auto prefix = "dynent " + std::to_string(index);
            const auto type = entity.at("type").get<int>();
            if (type != Clutter && type != Destructible)
                throw std::invalid_argument(prefix + ": unknown type " + std::to_string(type));
            const auto& model = entity.at("xModel").get_ref<const std::string&>();
            if (model.empty())
                throw std::invalid_argument(prefix + ": xmodel dynent without a model");
            const auto& pieces = entity.at("destroyPieces");
            if (!pieces.empty())
                throw std::invalid_argument(prefix + " (" + model + "): destroy pieces " +
                                            pieces.at("name").get<std::string>() + " are not converted yet");
            for (const auto& [field, size] : {std::pair{"quat", 4u}, std::pair{"origin", 3u}})
            {
                const auto& vector = entity.at(field);
                if (!vector.is_array() || vector.size() != size || !std::ranges::all_of(vector, [](const Json& value) {
                        return value.is_number() && std::isfinite(value.get<double>());
                    }))
                    throw std::invalid_argument(prefix + ": invalid " + field);
            }
        }
    }

    Json ReadDynents(const Path& source)
    {
        if (!std::filesystem::is_regular_file(source))
            return Json::array();
        auto data = weapons::ReadAssetJson(source);
        if (data.value("version", 0) != 1)
            throw std::invalid_argument("unsupported map dynent schema");
        auto& lists = data.at("lists");
        auto& models = lists.at(0);
        const auto brushes = std::ranges::count_if(lists.at(1), [](const Json& entry) { return entry.at("type") != 0; });
        if (brushes != 0)
            throw std::invalid_argument("map has " + std::to_string(brushes) + " brush-model dynents; only xmodel dynents are converted");
        const auto size = static_cast<std::int64_t>(models.size());
        auto count = data.at("originalDynEntCount").get<std::int64_t>();
        if (count < 0)
            count += size;
        const auto firstRuntime = models.begin() + std::clamp(count, std::int64_t{}, size);
        if (std::any_of(firstRuntime, models.end(), [](const Json& entry) { return entry.at("type") != 0; }))
            throw std::invalid_argument("map dynents beyond originalDynEntCount");
        models.erase(firstRuntime, models.end());
        for (std::size_t i = 0; i < models.size(); ++i)
            Validate(models.at(i), i);
        return std::move(models);
    }

    std::string PresetName(const Json& entity)
    {
        const auto& preset = entity.at("physPreset");
        if (!preset.is_object())
            return {};
        const auto field = preset.find("name");
        if (field == preset.end() || field->is_null())
            return {};
        const auto& name = field->get_ref<const std::string&>();
        const auto first = name.find_first_not_of(',');
        return first == name.npos ? std::string{} : name.substr(first);
    }

    Json WriteDynents(const Path& project, const Json& entries, const Json& presets, std::string_view effectPrefix)
    {
        const auto target = project / "BSP/dynents.json";
        if (entries.empty())
        {
            std::filesystem::remove(target);
            return {{"count", 0}};
        }
        auto output = Json::array();
        auto resting = Json::array();
        std::set<std::string> models;
        std::size_t clutter = 0, destructible = 0;
        for (const auto& entity : entries)
        {
            const auto& model = entity.at("xModel").get_ref<const std::string&>();
            models.insert(model);
            const auto type = entity.at("type").get<int>();
            clutter += type == Clutter;
            destructible += type == Destructible;
            const auto flags = entity.at("flags").get<std::int64_t>();
            // The T6 client needs converted model collision geometry to create a spawn body.
            if ((flags & SpawnBody) != 0)
                resting.push_back(model);
            auto constraints = entity.at("physConstraints");
            for (auto& index : constraints)
                if (index == 255)
                    index = -1;
            const auto& effect = entity.at("destroyFx");
            const auto destroyFx = effect.is_null() || effect.get_ref<const std::string&>().empty()
                ? std::string{} : std::string(effectPrefix) + effect.get_ref<const std::string&>();
            output.push_back({{"type", type}, {"quat", entity.at("quat")}, {"origin", entity.at("origin")}, {"xModel", model},
                              {"physPreset", presets.value(PresetName(entity), std::string{})},
                              {"destroyFx", destroyFx},
                              {"physConstraints", std::move(constraints)}, {"health", entity.at("health")},
                              {"flags", flags & ~SpawnBody}, {"contents", entity.at("contents")}, {"targetname", entity.at("targetname")}});
        }
        const auto count = output.size();
        files::WriteText(target, py::JsonDumps({{"version", 1}, {"dynents", std::move(output)}}, 1) + "\n");
        return {{"count", count}, {"models", models}, {"clutter", clutter}, {"destructible", destructible},
                {"resting", std::move(resting)}};
    }
}

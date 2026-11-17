#include "world/primary_lights.h"

#include "core/files.h"

#include <algorithm>
#include <set>

namespace w2b::world
{
    namespace
    {
        using py::Json;

        void AddLightFields(Json& light)
        {
            const auto radius = light.at("radius").get<double>();
            const auto near = std::min(1.0, radius * 0.01);
            const auto exponent = light.value("exponent", 0.0);
            light["falloff"] = {near, radius, near, 0.0};
            light["aAbB"] = {light.at("cosHalfFovInner").get<double>(), light.at("cosHalfFovOuter").get<double>(),
                             exponent > 0 ? 1.0 / exponent : 1e30, 0.0};
            light["dAttenuation"] = radius * radius * 1e4;
            light["roundness"] = 0.5;
        }

        bool Enabled(const Json& light, std::string_view key)
        {
            const auto it = light.find(key);
            if (it == light.end() || it->is_null())
                return false;
            return it->is_boolean() ? it->get<bool>() : it->get<double>() != 0;
        }

        Json Square(const Json& color)
        {
            if (color.is_boolean())
                return color.get<bool>() ? 1 : 0;
            if (color.is_number_integer())
            {
                const auto value = color.get<double>();
                if (value >= -3037000499.0 && value <= 3037000499.0)
                {
                    const auto integer = color.get<std::int64_t>();
                    return integer * integer;
                }
            }
            const auto value = color.get<double>();
            return value * value;
        }
    }

    py::Json StagePrimaryLights(const files::fs::path& source, const files::fs::path& destination, bool keepShadows)
    {
        auto data = Json::parse(files::ReadText(source));
        std::set<std::string> definitions;
        auto unshadowed = Json::array();
        for (auto& light : data.at("lights"))
        {
            auto type = light.at("type").get<int>();
            if (type < 0 || type > 3)
                return {{"error", "unsupported WaW primary light type " + std::to_string(type)}};
            if (type == 3)
            {
                type = 5;
                light["type"] = type;
            }
            if (type != 1 && light.contains("color"))
                for (auto& channel : light.at("color"))
                    channel = Square(channel);
            if (type > 1)
            {
                AddLightFields(light);
                if (!keepShadows && Enabled(light, "canUseShadowMap"))
                {
                    light["canUseShadowMap"] = 0;
                    unshadowed.push_back(light.value("index", Json{}));
                }
            }
            const auto& definition = light.at("defName");
            if (!definition.is_null() && !definition.get_ref<const std::string&>().empty())
            {
                const auto name = definition.get<std::string>();
                definitions.insert(name);
                light["defName"] = "waw_light/" + name;
            }
        }
        files::WriteText(destination, py::JsonDumps(data, 1) + "\n");
        return {{"definitions", definitions}, {"unshadowed", std::move(unshadowed)}};
    }
}

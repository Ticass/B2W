#include "effects/effect.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>

namespace w2b::effects
{
    namespace
    {
        using py::Json;

        unsigned ConvertFlags(unsigned flags, std::vector<std::string>& notes, const std::string& location)
        {
            // T6 inserts one bit between the run mode and useCollision.
            constexpr unsigned same = 0xdf0000fe;
            constexpr unsigned shifted = 0x00001f00;
            if (const auto unknown = flags & ~(same | shifted))
                notes.push_back(std::format("{}: unknown T4 element flag bits 0x{:08x} dropped", location, unknown));
            return (flags & same) | ((flags & shifted) << 1);
        }

        Json ConvertState(const Json& source, bool light)
        {
            auto state = source;
            auto& color = state.at("color");
            std::swap(color.at(0), color.at(2)); // BGRA -> RGBA
            if (const auto alpha = color.at(3).get<int>(); light && alpha)
            {
                // Converted light intensities use WaW's effective exposure.
                const auto scaled = std::nearbyint(alpha / 32.0);
                color[3] = std::max(1, static_cast<int>(scaled));
            }
            return state;
        }

        std::string LookupMaterial(const Json& materials, const std::string& name)
        {
            return materials.value(name, name);
        }

        std::string Prefix(std::string_view prefix, const std::string& name)
        {
            return name.empty() ? name : std::string(prefix) + name;
        }

        Json ConvertVisual(const Json& source, const Json& materials, int type,
                           std::vector<std::string>& notes, const std::string& location)
        {
            if (const auto entries = source.find("materials"); entries != source.end())
            {
                auto names = Json::array();
                for (const auto& entry : *entries)
                    names.push_back(entry.is_null() || entry == "" ? "" :
                                    LookupMaterial(materials, entry.get<std::string>()));
                return {{"materials", std::move(names)}};
            }
            if (const auto entry = source.find("material"); entry != source.end())
            {
                const auto name = entry->get<std::string>();
                const auto variant = name + "#trail";
                return {{"material", LookupMaterial(materials, type == trail && materials.contains(variant) ? variant : name)}};
            }
            if (const auto effect = source.find("effect"); effect != source.end())
                return {{"effect", effect->is_null() ? "" : Prefix(outputPrefix, effect->get<std::string>())}};
            if (const auto alias = source.value("sound", Json{}); alias.is_string() && alias != "")
            {
                const auto name = alias.get<std::string>();
                notes.push_back("UNSUPPORTED_SOUND " + name + " (" + location +
                                " sound element): WaW sound aliases are not converted yet (silent)");
                return {{"sound", Prefix(outputPrefix, name)}};
            }
            if (const auto modelName = source.value("model", Json{}); modelName.is_string() && modelName != "")
                return {{"model", Prefix(modelPrefix, modelName.get<std::string>())}};
            return source;
        }
    }

    py::Json ConvertElement(const py::Json& source, const py::Json& materials,
                            std::vector<std::string>& notes, const std::string& location)
    {
        const auto sourceType = source.at("elemType").get<int>();
        constexpr std::array types{0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        if (sourceType < 0 || static_cast<std::size_t>(sourceType) >= types.size())
            throw EffectError(location + ": unknown T4 element type " + std::to_string(sourceType));
        const auto type = types[sourceType];
        if (type == spotLight)
            throw EffectError(location + ": spot light elements are not converted yet");

        auto result = Json::object();
        for (const std::string_view name : {"spawnLooping", "spawnOneShot", "spawnRange", "fadeInRange",
                "fadeOutRange", "spawnFrustumCullRadius", "spawnDelayMsec", "lifeSpanMsec", "spawnOrigin",
                "spawnOffsetRadius", "spawnOffsetHeight", "spawnAngles", "angularVelocity", "initialRotation",
                "gravity", "reflectionFactor", "windInfluence", "visualCount", "velIntervalCount",
                "visStateIntervalCount", "velSamples", "collMins", "collMaxs", "emitDist", "emitDistVariance",
                "sortOrder", "lightingFrac"})
            result[name] = source.at(name);
        result["flags"] = ConvertFlags(source.at("flags").get<unsigned>(), notes, location);
        result["elemType"] = type;
        result["rotationAxis"] = 0;
        const auto& atlas = source.at("atlas");
        auto outputAtlas = Json::object();
        for (const std::string_view name : {"behavior", "index", "fps", "loopCount", "colIndexBits", "rowIndexBits"})
            outputAtlas[name] = atlas.at(name);
        const auto entries = atlas.at("entryCount").get<unsigned>();
        outputAtlas["entryCountAndIndexRange"] = entries ? entries | 512 : 0;
        result["atlas"] = std::move(outputAtlas);

        auto samples = Json::array();
        for (const auto& sample : source.at("visSamples"))
            samples.push_back({{"base", ConvertState(sample.at("base"), type == omniLight)},
                               {"amplitude", ConvertState(sample.at("amplitude"), type == omniLight)}});
        result["visSamples"] = std::move(samples);
        auto visuals = Json::array();
        for (const auto& visual : source.at("visuals"))
            visuals.push_back(ConvertVisual(visual, materials, type, notes, location));
        result["visuals"] = std::move(visuals);
        for (const std::string_view name : {"effectOnImpact", "effectOnDeath", "effectEmitted"})
        {
            const auto& value = source.at(name);
            result[name] = value.is_null() ? "" : Prefix(outputPrefix, value.get<std::string>());
        }
        result["effectAttached"] = "";
        result["trail"] = type == trail ? source.at("trail") : Json{};
        if (type == trail && result.at("trail").empty())
            throw EffectError(location + ": trail element without trail data");
        result["spotLight"] = nullptr;
        result["unused"] = {0, 0};
        for (const std::string_view name : {"alphaFadeTimeMsec", "maxWindStrength", "spawnIntervalAtMaxWind",
                "lifespanAtMaxWind"})
            result[name] = 0;
        constexpr auto trim = std::bit_cast<std::uint32_t>(1.0f);
        result["u"] = type == cloud ? Json{1024, 0} : Json{trim, trim};
        result["spawnSound"] = "";
        result["billboardPivot"] = {0.0, 0.0};
        return result;
    }
}

#include "materials/material.h"

#include "materials/techsets.h"

#include <array>
#include <map>

namespace w2b::materials
{
    namespace
    {
        using py::Json;

        const Json& Get(const Json& object, std::string_view name)
        {
            static const Json missing;
            const auto it = object.find(name);
            return it == object.end() ? missing : *it;
        }

        std::string Text(const Json& object, std::string_view name)
        {
            const auto& field = Get(object, name);
            return field.is_string() ? field.get<std::string>() : std::string{};
        }

        std::string Describe(const Json& value)
        {
            if (value.is_null())
                return "None";
            return value.is_string() ? value.get<std::string>() : value.dump();
        }

        bool True(const Json& value)
        {
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_number())
                return value.get<double>() != 0;
            if (value.is_string())
                return !value.get_ref<const std::string&>().empty();
            return !value.empty();
        }

        bool ColorPass(const Json& state)
        {
            return True(Get(state, "colorWriteRgb")) && !True(Get(state, "polymodeLine"));
        }

        const Json* UnlitState(const Json& source)
        {
            const auto& entries = Get(source, "stateBitsEntry");
            const auto& states = Get(source, "stateBits");
            const auto index = entries.size() > 4 ? entries[4].get<int>() : -1;
            return index >= 0 && static_cast<std::size_t>(index) < states.size() ? &states[index] : nullptr;
        }

        void ApplySourceCull(Json& material, const Json& source)
        {
            bool found = false;
            for (const auto& state : Get(source, "stateBits"))
            {
                if (!ColorPass(state))
                    continue;
                if (Get(state, "cullFace") != "none")
                    return;
                found = true;
            }
            if (found && material.contains("stateBits"))
                for (auto& state : material["stateBits"])
                    if (state.contains("cullFace"))
                        state["cullFace"] = "none";
        }

        void ApplyUnlitState(Json& material, const Json& source)
        {
            const auto technique = ParseTechnique(Text(source, "techniqueSet"));
            if (technique.family != Family::world ||
                (technique.unlit != "unlit" && technique.unlit != "unlit_blend" &&
                 technique.unlit != "unlit_distfalloff" && technique.unlit != "unlit_falloff_add"))
                return;
            const auto main = UnlitState(source);
            if (!main)
                return;
            constexpr std::array<std::string_view, 13> fields{"srcBlendRgb", "dstBlendRgb", "blendOpRgb",
                "srcBlendAlpha", "dstBlendAlpha", "blendOpAlpha", "alphaTest", "depthWrite", "depthTest",
                "cullFace", "polygonOffset", "colorWriteAlpha", "colorWriteRgb"};
            if (material.contains("stateBits"))
                for (auto& state : material["stateBits"])
                    if (ColorPass(state))
                        for (const auto field : fields)
                            if (const auto it = main->find(field); it != main->end())
                                state[field] = *it;
            if (source.contains("sortKey"))
                material["sortKey"] = source["sortKey"];
        }

        void ApplyFlags(Json& material, const Json& source)
        {
            auto flags = Json::array();
            for (const auto& flag : Get(material, "gameFlags"))
                if (flag != "4" && flag != "NO_MARKS")
                    flags.push_back(flag);
            for (const auto& flag : Get(source, "gameFlags"))
                if (flag == "4")
                {
                    flags.push_back("NO_MARKS");
                    break;
                }
            material["gameFlags"] = std::move(flags);
        }

        Json BindTextures(const Json& donor, const Json& source, std::vector<std::string>& notes)
        {
            const auto& sourceTextures = Get(source, "textures");
            std::map<std::string, const Json*> byName;
            for (const auto& texture : sourceTextures)
                if (True(Get(texture, "name")) && True(Get(texture, "image")))
                    byName[Text(texture, "name")] = &texture;
            auto textures = Json::array();
            std::set<std::string> used;
            for (const auto& slot : Get(donor, "textures"))
            {
                auto output = slot;
                const auto name = Text(slot, "name");
                if (const auto found = byName.find(name); found != byName.end())
                {
                    const auto& input = *found->second;
                    auto image = Text(input, "image");
                    if (image.starts_with('$'))
                        image.insert(0, ",");
                    output["image"] = std::move(image);
                    const auto& sampler = Get(input, "samplerState");
                    for (const auto key : {"clampU", "clampV", "clampW", "filter", "mipMap"})
                        if (sampler.contains(key))
                            output["samplerState"][key] = sampler[key];
                    used.insert(name);
                }
                else if (Get(slot, "semantic") == "normalMap")
                    output["image"] = ",$identitynormalmap";
                else
                    throw TechniqueError("donor slot '" + Describe(Get(slot, "name")) + "' (" +
                                         Describe(Get(slot, "semantic")) + ") has no source texture");
                textures.push_back(std::move(output));
            }
            std::string dropped;
            for (const auto& texture : sourceTextures)
            {
                const auto image = Text(texture, "image");
                const auto name = Text(texture, "name");
                if (image.empty() || used.contains(name) ||
                    (image == "$identitynormalmap" && Get(texture, "semantic") == "normalMap"))
                    continue;
                if (!dropped.empty())
                    dropped += ", ";
                dropped += (name.empty() ? Describe(Get(texture, "semantic")) : name) + ":" + image;
            }
            if (!dropped.empty())
                notes.push_back("source textures without a T6 slot: " + dropped);
            return textures;
        }

        Json BindConstants(const Json& donor, const Json& source)
        {
            std::map<std::string, const Json*> literals;
            for (const auto& constant : Get(source, "constants"))
                if (True(Get(constant, "name")))
                    literals[Text(constant, "name")] = &constant;
            auto out = Json::array();
            for (auto constant : Get(donor, "constants"))
            {
                const auto name = Text(constant, "name");
                if (const auto input = literals.find(name); input != literals.end())
                    constant["literal"] = input->second->at("literal");
                else if (name.starts_with("colorTint") || name.starts_with("occlusionAmount") || name.starts_with("scaleRGB"))
                    constant["literal"] = {1.0, 1.0, 1.0, 1.0};
                out.push_back(std::move(constant));
            }
            return out;
        }
    }

    std::string SourceTechnique(const py::Json& source)
    {
        auto name = Text(source, "techniqueSet");
        const auto parsed = ParseTechnique(name);
        if ((parsed.family == Family::world || parsed.family == Family::model) && parsed.unlit == "unlit")
            if (const auto state = UnlitState(source);
                state && state->contains("blendOpRgb") && Get(*state, "blendOpRgb") != "disabled")
                name += parsed.family == Family::model && Get(*state, "dstBlendRgb") == "one" ? "_add" : "_blend";
        return name;
    }

    py::Json BuildMaterial(const py::Json& source, const py::Json& donor, std::vector<std::string>& notes)
    {
        auto material = donor;
        material.erase("thermalMaterial");
        ApplyFlags(material, source);
        material["textures"] = BindTextures(donor, source, notes);
        material["constants"] = BindConstants(donor, source);
        ApplySourceCull(material, source);
        ApplyUnlitState(material, source);
        material["_game"] = "t6";
        material["_type"] = "material";
        return material;
    }
}

#include "materials/material.h"

#include "materials/techsets.h"

#include <array>
#include <map>
#include <set>

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

        // an enum-like state value as T6 material files hold it (empty: null)
        Json StateValue(const std::string& value)
        {
            return value.empty() ? Json() : Json(value);
        }

        bool ColorPass(const WawStateBits& state)
        {
            return state.colorWriteRgb && !state.polymodeLine;
        }

        bool ColorPass(const Json& state)
        {
            const auto& rgb = Get(state, "colorWriteRgb");
            const auto& line = Get(state, "polymodeLine");
            return rgb.is_boolean() && rgb.get<bool>() && !(line.is_boolean() && line.get<bool>());
        }

        const WawStateBits* UnlitState(const WawMaterial& source)
        {
            return source.TechniqueState(WawMaterial::UNLIT_PASS);
        }

        // a WaW material drawn double-sided stays double-sided
        void ApplySourceCull(Json& material, const WawMaterial& source)
        {
            bool found = false;
            for (const auto& state : source.stateBits)
            {
                if (!ColorPass(state))
                    continue;
                if (state.cullFace != "none")
                    return;
                found = true;
            }
            if (found && material.contains("stateBits"))
                for (auto& state : material["stateBits"])
                    if (state.contains("cullFace"))
                        state["cullFace"] = "none";
        }

        void ApplyUnlitState(Json& material, const WawMaterial& source)
        {
            const auto technique = ParseTechnique(source.techniqueSet);
            if (technique.family != Family::world ||
                (technique.unlit != "unlit" && technique.unlit != "unlit_blend" && technique.unlit != "unlit_distfalloff" && technique.unlit != "unlit_falloff_add"))
                return;
            const auto* main = UnlitState(source);
            if (!main)
                return;
            const std::array<std::pair<const char*, Json>, 13> fields{{{"srcBlendRgb", main->srcBlendRgb},
                                                                       {"dstBlendRgb", main->dstBlendRgb},
                                                                       {"blendOpRgb", main->blendOpRgb},
                                                                       {"srcBlendAlpha", main->srcBlendAlpha},
                                                                       {"dstBlendAlpha", main->dstBlendAlpha},
                                                                       {"blendOpAlpha", main->blendOpAlpha},
                                                                       {"alphaTest", StateValue(main->alphaTest)},
                                                                       {"depthWrite", main->depthWrite},
                                                                       {"depthTest", StateValue(main->depthTest)},
                                                                       {"cullFace", StateValue(main->cullFace)},
                                                                       {"polygonOffset", main->polygonOffset},
                                                                       {"colorWriteAlpha", main->colorWriteAlpha},
                                                                       {"colorWriteRgb", main->colorWriteRgb}}};
            if (material.contains("stateBits"))
                for (auto& state : material["stateBits"])
                    if (ColorPass(state))
                        for (const auto& [field, value] : fields)
                            state[field] = value;
            material["sortKey"] = source.sortKey;
        }

        void ApplyFlags(Json& material, const WawMaterial& source)
        {
            auto flags = Json::array();
            for (const auto& flag : Get(material, "gameFlags"))
                if (flag != "4" && flag != "NO_MARKS")
                    flags.push_back(flag);
            if (source.gameFlags & WawMaterial::GAME_FLAG_NO_MARKS)
                flags.push_back("NO_MARKS");
            material["gameFlags"] = std::move(flags);
        }

        Json BindTextures(const Json& donor, const WawMaterial& source, std::vector<std::string>& notes)
        {
            std::map<std::string, const WawTexture*> byName;
            for (const auto& texture : source.textures)
                if (texture.name && !texture.name->empty() && !texture.image.empty())
                    byName[*texture.name] = &texture;
            auto textures = Json::array();
            std::set<std::string> used;
            for (const auto& slot : Get(donor, "textures"))
            {
                auto output = slot;
                const auto name = Text(slot, "name");
                if (const auto found = byName.find(name); found != byName.end())
                {
                    const auto& input = *found->second;
                    auto image = input.image;
                    if (image.starts_with('$'))
                        image.insert(0, ",");
                    output["image"] = std::move(image);
                    auto& sampler = output["samplerState"];
                    sampler["clampU"] = input.sampler.clampU;
                    sampler["clampV"] = input.sampler.clampV;
                    sampler["clampW"] = input.sampler.clampW;
                    sampler["filter"] = input.sampler.filter;
                    sampler["mipMap"] = input.sampler.mipMap;
                    used.insert(name);
                }
                else if (Get(slot, "semantic") == "normalMap")
                    output["image"] = ",$identitynormalmap";
                else
                    throw TechniqueError("donor slot '" + Describe(Get(slot, "name")) + "' (" + Describe(Get(slot, "semantic")) + ") has no source texture");
                textures.push_back(std::move(output));
            }
            std::string dropped;
            for (const auto& texture : source.textures)
            {
                const auto name = texture.name.value_or("");
                if (texture.image.empty() || used.contains(name) || (texture.image == "$identitynormalmap" && texture.semantic == "normalMap"))
                    continue;
                if (!dropped.empty())
                    dropped += ", ";
                dropped += (name.empty() ? texture.semantic : name) + ":" + texture.image;
            }
            if (!dropped.empty())
                notes.push_back("source textures without a T6 slot: " + dropped);
            return textures;
        }

        Json BindConstants(const Json& donor, const WawMaterial& source)
        {
            std::map<std::string, const WawConstant*> literals;
            for (const auto& constant : source.constants)
                if (constant.name && !constant.name->empty())
                    literals[*constant.name] = &constant;
            auto out = Json::array();
            for (auto constant : Get(donor, "constants"))
            {
                const auto name = Text(constant, "name");
                if (const auto input = literals.find(name); input != literals.end())
                    constant["literal"] = input->second->literal;
                else if (name.starts_with("colorTint") || name.starts_with("occlusionAmount") || name.starts_with("scaleRGB"))
                    constant["literal"] = {1.0, 1.0, 1.0, 1.0};
                out.push_back(std::move(constant));
            }
            return out;
        }
    } // namespace

    std::string SourceTechnique(const WawMaterial& source)
    {
        auto name = source.techniqueSet;
        const auto parsed = ParseTechnique(name);
        if ((parsed.family == Family::world || parsed.family == Family::model) && parsed.unlit == "unlit")
            if (const auto* state = UnlitState(source); state && state->blendOpRgb != "disabled")
                name += parsed.family == Family::model && state->dstBlendRgb == "one" ? "_add" : "_blend";
        return name;
    }

    py::Json BuildMaterial(const WawMaterial& source, const py::Json& donor, std::vector<std::string>& notes)
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
} // namespace w2b::materials

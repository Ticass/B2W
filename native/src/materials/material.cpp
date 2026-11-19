#include "materials/material.h"

#include "materials/techsets.h"

#include <map>
#include <set>

namespace w2b::materials
{
    namespace
    {
        bool ColorPass(const WawStateBits& state)
        {
            return state.colorWriteRgb && !state.polymodeLine;
        }

        bool ColorPass(const T6StateBits& state)
        {
            return state.colorWriteRgb && !state.polymodeLine;
        }

        const WawStateBits* UnlitState(const WawMaterial& source)
        {
            return source.TechniqueState(WawMaterial::UNLIT_PASS);
        }

        // a WaW material drawn double-sided stays double-sided
        void ApplySourceCull(T6Material& material, const WawMaterial& source)
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
            if (found)
                for (auto& state : material.stateBits)
                    state.cullFace = "none";
        }

        void ApplyUnlitState(T6Material& material, const WawMaterial& source)
        {
            const auto technique = ParseTechnique(source.techniqueSet);
            if (technique.family != Family::world ||
                (technique.unlit != "unlit" && technique.unlit != "unlit_blend" && technique.unlit != "unlit_distfalloff" && technique.unlit != "unlit_falloff_add"))
                return;
            const auto* main = UnlitState(source);
            if (!main)
                return;
            const auto wanted = ToT6(*main);
            for (auto& state : material.stateBits)
                if (ColorPass(state))
                {
                    CopyBlendState(state, wanted);
                    CopyRasterState(state, wanted);
                }
            material.sortKey = source.sortKey;
        }

        void ApplyFlags(T6Material& material, const WawMaterial& source)
        {
            std::erase_if(material.gameFlags, [](const std::string& flag) { return flag == "4" || flag == "NO_MARKS"; });
            if (source.gameFlags & WawMaterial::GAME_FLAG_NO_MARKS)
                material.gameFlags.emplace_back("NO_MARKS");
        }

        std::vector<T6Texture> BindTextures(const T6Material& donor, const WawMaterial& source, std::vector<std::string>& notes)
        {
            std::map<std::string, const WawTexture*> byName;
            for (const auto& texture : source.textures)
                if (texture.name && !texture.name->empty() && !texture.image.empty())
                    byName[*texture.name] = &texture;
            std::vector<T6Texture> textures;
            std::set<std::string> used;
            for (const auto& slot : donor.textures)
            {
                auto output = slot;
                const auto name = slot.name.value_or("");
                if (const auto found = byName.find(name); found != byName.end())
                {
                    const auto& input = *found->second;
                    auto image = input.image;
                    if (image.starts_with('$'))
                        image.insert(0, ",");
                    output.image = std::move(image);
                    output.sampler = {input.sampler.filter, input.sampler.mipMap, input.sampler.clampU, input.sampler.clampV, input.sampler.clampW};
                    used.insert(name);
                }
                else if (slot.semantic == "normalMap")
                    output.image = ",$identitynormalmap";
                else
                    throw TechniqueError("donor slot '" + (slot.name ? *slot.name : std::string("None")) + "' (" + slot.semantic + ") has no source texture");
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

        std::vector<T6Constant> BindConstants(const T6Material& donor, const WawMaterial& source)
        {
            std::map<std::string, const WawConstant*> literals;
            for (const auto& constant : source.constants)
                if (constant.name && !constant.name->empty())
                    literals[*constant.name] = &constant;
            auto out = donor.constants;
            for (auto& constant : out)
            {
                const auto name = constant.name.value_or("");
                if (const auto input = literals.find(name); input != literals.end())
                    for (std::size_t i = 0; i < 4; ++i)
                        constant.literal[i] = input->second->literal[i];
                else if (name.starts_with("colorTint") || name.starts_with("occlusionAmount") || name.starts_with("scaleRGB"))
                    constant.literal = {1.0, 1.0, 1.0, 1.0};
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

    T6Material BuildMaterial(const WawMaterial& source, const T6Material& donor, std::vector<std::string>& notes)
    {
        auto material = donor;
        material.thermalMaterial.reset();
        ApplyFlags(material, source);
        material.textures = BindTextures(donor, source, notes);
        material.constants = BindConstants(donor, source);
        ApplySourceCull(material, source);
        ApplyUnlitState(material, source);
        return material;
    }
} // namespace w2b::materials

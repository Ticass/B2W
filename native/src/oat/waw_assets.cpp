// WaW assets as objects (see waw_assets.h).
#include "oat/waw_assets.h"

#include "oat/loaded_zones.h"
#include "oat/oat_state.h"

#include "Game/T4/Material/JsonMaterialT4.h"
#include "Game/T4/Material/MaterialJsonDumperT4.h"
#include "Game/T4/T4.h"

namespace w2b::oat
{
    namespace
    {
        // OAT's name for an enum value, as its JSON files spell it; empty for
        // a value it writes as null
        template<typename Enum> std::string Name(const Enum value)
        {
            const nlohmann::json named = value;
            return named.is_string() ? named.get<std::string>() : std::string{};
        }

        materials::WawStencil Stencil(const T4::JsonStencil& stencil)
        {
            return {Name(stencil.pass), Name(stencil.fail), Name(stencil.zfail), Name(stencil.func)};
        }

        materials::WawStateBits State(const T4::JsonStateBitsTableEntry& entry)
        {
            materials::WawStateBits state;
            state.srcBlendRgb = Name(entry.srcBlendRgb);
            state.dstBlendRgb = Name(entry.dstBlendRgb);
            state.blendOpRgb = Name(entry.blendOpRgb);
            state.alphaTest = Name(entry.alphaTest);
            state.cullFace = Name(entry.cullFace);
            state.srcBlendAlpha = Name(entry.srcBlendAlpha);
            state.dstBlendAlpha = Name(entry.dstBlendAlpha);
            state.blendOpAlpha = Name(entry.blendOpAlpha);
            state.colorWriteRgb = entry.colorWriteRgb;
            state.colorWriteAlpha = entry.colorWriteAlpha;
            state.polymodeLine = entry.polymodeLine;
            state.depthWrite = entry.depthWrite;
            state.depthTest = Name(entry.depthTest);
            state.polygonOffset = Name(entry.polygonOffset);
            if (entry.stencilFront)
                state.stencilFront = Stencil(*entry.stencilFront);
            if (entry.stencilBack)
                state.stencilBack = Stencil(*entry.stencilBack);
            return state;
        }

        materials::WawTexture Texture(const T4::JsonTexture& entry)
        {
            materials::WawTexture texture;
            texture.name = entry.name;
            if (!entry.name)
            {
                texture.nameHash = entry.nameHash;
                texture.nameStart = entry.nameStart.value_or("");
                texture.nameEnd = entry.nameEnd.value_or("");
            }
            texture.semantic = Name(entry.semantic);
            texture.matureContent = entry.isMatureContent;
            texture.sampler = {Name(entry.samplerState.filter), Name(entry.samplerState.mipMap), entry.samplerState.clampU, entry.samplerState.clampV,
                               entry.samplerState.clampW};
            texture.image = entry.image;
            if (entry.water)
            {
                const auto& water = *entry.water;
                texture.water = materials::WawWater{water.floatTime, water.m,       water.n,       water.h0,        water.wTerm,     water.lx,
                                                    water.lz,        water.gravity, water.windvel, water.winddir, water.amplitude, water.codeConstant};
            }
            return texture;
        }

        materials::WawConstant Constant(const T4::JsonConstant& entry)
        {
            materials::WawConstant constant;
            constant.name = entry.name;
            if (!entry.name)
            {
                constant.nameFragment = entry.nameFragment.value_or("");
                constant.nameHash = entry.nameHash;
            }
            constant.literal = entry.literal;
            return constant;
        }

        materials::WawMaterial Material(const T4::JsonMaterial& entry)
        {
            materials::WawMaterial material;
            material.techniqueSet = entry.techniqueSet;
            for (const auto flag : entry.gameFlags)
                material.gameFlags |= static_cast<unsigned>(flag);
            material.sortKey = entry.sortKey;
            if (entry.textureAtlas)
            {
                material.atlasRows = entry.textureAtlas->rows;
                material.atlasColumns = entry.textureAtlas->columns;
            }
            material.surfaceTypeBits = entry.surfaceTypeBits;
            material.stateBitsEntry.assign(entry.stateBitsEntry.begin(), entry.stateBitsEntry.end());
            material.stateFlags = entry.stateFlags;
            material.cameraRegion = Name(entry.cameraRegion);
            for (const auto& texture : entry.textures)
                material.textures.push_back(Texture(texture));
            for (const auto& constant : entry.constants)
                material.constants.push_back(Constant(constant));
            for (const auto& state : entry.stateBits)
                material.stateBits.push_back(State(state));
            return material;
        }
    } // namespace

    std::optional<materials::WawMaterial> ReadMaterial(const WawZone& zone, const std::string& name)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput quiet; // loading logs every IWD it opens
        auto& loaded = Loaded(zone.fastfile, zone.searchPaths);
        const auto* asset = loaded.Get().m_pools.GetAsset<T4::AssetMaterial>(name);
        if (!asset || asset->IsReference())
            return std::nullopt;
        return Material(material::JsonDumperT4::Create(loaded.ReadingContext(), *asset->Asset()));
    }

    std::vector<std::string> MaterialNames(const WawZone& zone)
    {
        const std::lock_guard guard(StateLock());
        const CapturedOutput quiet;
        std::vector<std::string> names;
        for (const auto* asset : Loaded(zone.fastfile, zone.searchPaths).Get().m_pools.PoolAssets<T4::AssetMaterial>())
            if (!asset->IsReference())
                names.push_back(asset->m_name);
        return names;
    }
} // namespace w2b::oat

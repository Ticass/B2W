// WaW materials as objects (see waw_material.h).
#include "materials/waw_material_json.h"

#include <format>

namespace w2b::materials
{
    namespace
    {
        using py::Json;
        using SortedJson = nlohmann::json; // OAT writes its files with sorted keys

        std::string TextOrEmpty(const Json& value)
        {
            return value.is_string() ? value.get<std::string>() : std::string{};
        }

        // OAT's T4 names for the game flags: hex digits, except 0x40
        constexpr unsigned CASTS_SHADOW = 0x40;

        std::string FlagName(const unsigned flag)
        {
            return flag == CASTS_SHADOW ? std::string("CASTS_SHADOW") : std::format("{:x}", flag);
        }

        unsigned FlagValue(const std::string& name)
        {
            return name == "CASTS_SHADOW" ? CASTS_SHADOW : static_cast<unsigned>(std::stoul(name, nullptr, 16));
        }

        template<std::size_t N> std::array<float, N> Floats(const Json& values)
        {
            std::array<float, N> out{};
            for (std::size_t i = 0; i < N; i++)
                out[i] = values.at(i).get<float>();
            return out;
        }

        // the readers' key order is the file's, which is sorted
        Json Ordered(const SortedJson& sorted)
        {
            return Json::parse(sorted.dump());
        }

        SortedJson TextOrNull(const std::string& value)
        {
            return value.empty() ? SortedJson() : SortedJson(value);
        }

        WawStencil StencilFromJson(const Json& data)
        {
            return {data.at("pass").get<std::string>(), data.at("fail").get<std::string>(), data.at("zfail").get<std::string>(),
                    data.at("func").get<std::string>()};
        }

        SortedJson StencilToJson(const WawStencil& stencil)
        {
            return {{"pass", stencil.pass}, {"fail", stencil.fail}, {"zfail", stencil.zfail}, {"func", stencil.func}};
        }

        WawStateBits StateFromJson(const Json& data)
        {
            WawStateBits state;
            state.srcBlendRgb = data.at("srcBlendRgb").get<std::string>();
            state.dstBlendRgb = data.at("dstBlendRgb").get<std::string>();
            state.blendOpRgb = data.at("blendOpRgb").get<std::string>();
            state.alphaTest = TextOrEmpty(data.at("alphaTest"));
            state.cullFace = TextOrEmpty(data.at("cullFace"));
            state.srcBlendAlpha = data.at("srcBlendAlpha").get<std::string>();
            state.dstBlendAlpha = data.at("dstBlendAlpha").get<std::string>();
            state.blendOpAlpha = data.at("blendOpAlpha").get<std::string>();
            state.colorWriteRgb = data.at("colorWriteRgb").get<bool>();
            state.colorWriteAlpha = data.at("colorWriteAlpha").get<bool>();
            state.polymodeLine = data.at("polymodeLine").get<bool>();
            state.depthWrite = data.at("depthWrite").get<bool>();
            state.depthTest = TextOrEmpty(data.at("depthTest"));
            state.polygonOffset = data.at("polygonOffset").get<std::string>();
            if (const auto it = data.find("stencilFront"); it != data.end() && !it->is_null())
                state.stencilFront = StencilFromJson(*it);
            if (const auto it = data.find("stencilBack"); it != data.end() && !it->is_null())
                state.stencilBack = StencilFromJson(*it);
            return state;
        }

        SortedJson StateToJson(const WawStateBits& state)
        {
            SortedJson out{{"srcBlendRgb", state.srcBlendRgb},
                           {"dstBlendRgb", state.dstBlendRgb},
                           {"blendOpRgb", state.blendOpRgb},
                           {"alphaTest", TextOrNull(state.alphaTest)},
                           {"cullFace", TextOrNull(state.cullFace)},
                           {"srcBlendAlpha", state.srcBlendAlpha},
                           {"dstBlendAlpha", state.dstBlendAlpha},
                           {"blendOpAlpha", state.blendOpAlpha},
                           {"colorWriteRgb", state.colorWriteRgb},
                           {"colorWriteAlpha", state.colorWriteAlpha},
                           {"polymodeLine", state.polymodeLine},
                           {"depthWrite", state.depthWrite},
                           {"depthTest", TextOrNull(state.depthTest)},
                           {"polygonOffset", state.polygonOffset}};
            if (state.stencilFront)
                out["stencilFront"] = StencilToJson(*state.stencilFront);
            if (state.stencilBack)
                out["stencilBack"] = StencilToJson(*state.stencilBack);
            return out;
        }

        WawWater WaterFromJson(const Json& data)
        {
            WawWater water;
            water.floatTime = data.at("floatTime").get<float>();
            water.m = data.at("m").get<int>();
            water.n = data.at("n").get<int>();
            water.h0 = data.at("h0").get<std::string>();
            water.wTerm = data.at("wTerm").get<std::string>();
            water.lx = data.at("lx").get<float>();
            water.lz = data.at("lz").get<float>();
            water.gravity = data.at("gravity").get<float>();
            water.windvel = data.at("windvel").get<float>();
            water.winddir = Floats<2>(data.at("winddir"));
            water.amplitude = data.at("amplitude").get<float>();
            water.codeConstant = Floats<4>(data.at("codeConstant"));
            return water;
        }

        SortedJson WaterToJson(const WawWater& water)
        {
            return {{"floatTime", water.floatTime}, {"m", water.m},         {"n", water.n},
                    {"h0", water.h0},               {"wTerm", water.wTerm}, {"lx", water.lx},
                    {"lz", water.lz},               {"gravity", water.gravity}, {"windvel", water.windvel},
                    {"winddir", water.winddir},     {"amplitude", water.amplitude}, {"codeConstant", water.codeConstant}};
        }

        WawTexture TextureFromJson(const Json& data)
        {
            WawTexture texture;
            if (const auto it = data.find("name"); it != data.end() && !it->is_null())
                texture.name = it->get<std::string>();
            if (const auto it = data.find("nameHash"); it != data.end() && !it->is_null())
                texture.nameHash = it->get<std::uint32_t>();
            texture.nameStart = TextOrEmpty(data.value("nameStart", Json()));
            texture.nameEnd = TextOrEmpty(data.value("nameEnd", Json()));
            texture.semantic = data.at("semantic").get<std::string>();
            texture.matureContent = data.at("isMatureContent").get<bool>();
            const auto& sampler = data.at("samplerState");
            texture.sampler = {sampler.at("filter").get<std::string>(), sampler.at("mipMap").get<std::string>(), sampler.at("clampU").get<bool>(),
                               sampler.at("clampV").get<bool>(), sampler.at("clampW").get<bool>()};
            texture.image = data.at("image").get<std::string>();
            if (const auto it = data.find("water"); it != data.end() && !it->is_null())
                texture.water = WaterFromJson(*it);
            return texture;
        }

        SortedJson TextureToJson(const WawTexture& texture)
        {
            SortedJson out = SortedJson::object();
            if (texture.name)
                out["name"] = *texture.name;
            else
            {
                if (texture.nameHash)
                    out["nameHash"] = *texture.nameHash;
                out["nameStart"] = texture.nameStart;
                out["nameEnd"] = texture.nameEnd;
            }
            out["semantic"] = texture.semantic;
            out["isMatureContent"] = texture.matureContent;
            out["samplerState"] = {{"filter", texture.sampler.filter},
                                   {"mipMap", texture.sampler.mipMap},
                                   {"clampU", texture.sampler.clampU},
                                   {"clampV", texture.sampler.clampV},
                                   {"clampW", texture.sampler.clampW}};
            out["image"] = texture.image;
            if (texture.water)
                out["water"] = WaterToJson(*texture.water);
            return out;
        }

        WawConstant ConstantFromJson(const Json& data)
        {
            WawConstant constant;
            if (const auto it = data.find("name"); it != data.end() && !it->is_null())
                constant.name = it->get<std::string>();
            constant.nameFragment = TextOrEmpty(data.value("nameFragment", Json()));
            if (const auto it = data.find("nameHash"); it != data.end() && !it->is_null())
                constant.nameHash = it->get<std::uint32_t>();
            constant.literal = Floats<4>(data.at("literal"));
            return constant;
        }

        SortedJson ConstantToJson(const WawConstant& constant)
        {
            SortedJson out = SortedJson::object();
            if (constant.name)
                out["name"] = *constant.name;
            else
            {
                out["nameFragment"] = constant.nameFragment;
                if (constant.nameHash)
                    out["nameHash"] = *constant.nameHash;
            }
            out["literal"] = constant.literal;
            return out;
        }
    } // namespace

    const WawStateBits* WawMaterial::TechniqueState(const std::size_t technique) const
    {
        if (technique >= stateBitsEntry.size())
            return nullptr;
        const auto index = stateBitsEntry[technique];
        return index >= 0 && static_cast<std::size_t>(index) < stateBits.size() ? &stateBits[index] : nullptr;
    }

    WawMaterial FromOatJson(const Json& data)
    {
        WawMaterial material;
        material.techniqueSet = TextOrEmpty(data.value("techniqueSet", Json()));
        for (const auto& flag : data.at("gameFlags"))
            material.gameFlags |= FlagValue(flag.get<std::string>());
        material.sortKey = data.at("sortKey").get<unsigned>();
        if (const auto it = data.find("textureAtlas"); it != data.end() && !it->is_null())
        {
            material.atlasRows = it->at("rows").get<unsigned>();
            material.atlasColumns = it->at("columns").get<unsigned>();
        }
        material.surfaceTypeBits = data.at("surfaceTypeBits").get<unsigned>();
        for (const auto& entry : data.at("stateBitsEntry"))
            material.stateBitsEntry.push_back(entry.get<int>());
        material.stateFlags = data.at("stateFlags").get<unsigned>();
        material.cameraRegion = data.at("cameraRegion").get<std::string>();
        for (const auto& texture : data.at("textures"))
            material.textures.push_back(TextureFromJson(texture));
        for (const auto& constant : data.at("constants"))
            material.constants.push_back(ConstantFromJson(constant));
        for (const auto& state : data.at("stateBits"))
            material.stateBits.push_back(StateFromJson(state));
        return material;
    }

    Json ToOatJson(const WawMaterial& material)
    {
        SortedJson flags = SortedJson::array();
        for (unsigned bit = 0; bit < 32; bit++)
            if (material.gameFlags & (1u << bit))
                flags.push_back(FlagName(1u << bit));
        SortedJson textures = SortedJson::array(), constants = SortedJson::array(), states = SortedJson::array();
        for (const auto& texture : material.textures)
            textures.push_back(TextureToJson(texture));
        for (const auto& constant : material.constants)
            constants.push_back(ConstantToJson(constant));
        for (const auto& state : material.stateBits)
            states.push_back(StateToJson(state));
        const SortedJson out{{"$schema", "http://openassettools.dev/schema/material.v1.json"},
                             {"_type", "material"},
                             {"_version", 1},
                             {"_game", "t4"},
                             {"gameFlags", flags},
                             {"sortKey", material.sortKey},
                             {"textureAtlas", {{"rows", material.atlasRows}, {"columns", material.atlasColumns}}},
                             {"surfaceTypeBits", material.surfaceTypeBits},
                             {"stateBitsEntry", material.stateBitsEntry},
                             {"stateFlags", material.stateFlags},
                             {"cameraRegion", material.cameraRegion},
                             {"techniqueSet", material.techniqueSet},
                             {"textures", textures},
                             {"constants", constants},
                             {"stateBits", states}};
        return Ordered(out);
    }

    Json ToOatJson(const WawStateBits& state)
    {
        return Ordered(StateToJson(state));
    }

    Json ToOatJson(const WawTexture& texture)
    {
        return Ordered(TextureToJson(texture));
    }
} // namespace w2b::materials

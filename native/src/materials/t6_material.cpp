#include "materials/t6_material.h"

#include "core/files.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace w2b::materials
{
    namespace
    {
        using py::Json;
        using Sorted = nlohmann::json; // std::map objects: the dumper's sorted key order

        template <typename T> std::optional<T> Optional(const Json& data, const char* key)
        {
            const auto it = data.find(key);
            return it == data.end() || it->is_null() ? std::nullopt : std::optional(it->get<T>());
        }

        T6Stencil StencilFromJson(const Json& data)
        {
            return {data.at("pass").get<std::string>(), data.at("fail").get<std::string>(), data.at("zfail").get<std::string>(),
                    data.at("func").get<std::string>()};
        }

        Sorted ToSorted(const T6Stencil& stencil)
        {
            return {{"pass", stencil.pass}, {"fail", stencil.fail}, {"zfail", stencil.zfail}, {"func", stencil.func}};
        }

        T6StateBits StateFromJson(const Json& data)
        {
            T6StateBits state;
            state.srcBlendRgb = data.at("srcBlendRgb").get<std::string>();
            state.dstBlendRgb = data.at("dstBlendRgb").get<std::string>();
            state.blendOpRgb = data.at("blendOpRgb").get<std::string>();
            state.alphaTest = Optional<std::string>(data, "alphaTest");
            state.cullFace = Optional<std::string>(data, "cullFace");
            state.srcBlendAlpha = data.at("srcBlendAlpha").get<std::string>();
            state.dstBlendAlpha = data.at("dstBlendAlpha").get<std::string>();
            state.blendOpAlpha = data.at("blendOpAlpha").get<std::string>();
            state.colorWriteRgb = data.at("colorWriteRgb").get<bool>();
            state.colorWriteAlpha = data.at("colorWriteAlpha").get<bool>();
            state.polymodeLine = data.at("polymodeLine").get<bool>();
            state.depthWrite = data.at("depthWrite").get<bool>();
            state.depthTest = Optional<std::string>(data, "depthTest");
            state.polygonOffset = data.at("polygonOffset").get<std::string>();
            if (const auto it = data.find("stencilFront"); it != data.end() && !it->is_null())
                state.stencilFront = StencilFromJson(*it);
            if (const auto it = data.find("stencilBack"); it != data.end() && !it->is_null())
                state.stencilBack = StencilFromJson(*it);
            return state;
        }

        Sorted Nullable(const std::optional<std::string>& value)
        {
            return value ? Sorted(*value) : Sorted();
        }

        Sorted ToSorted(const T6StateBits& state)
        {
            Sorted out{{"srcBlendRgb", state.srcBlendRgb},     {"dstBlendRgb", state.dstBlendRgb},     {"blendOpRgb", state.blendOpRgb},
                       {"alphaTest", Nullable(state.alphaTest)}, {"cullFace", Nullable(state.cullFace)},           {"srcBlendAlpha", state.srcBlendAlpha},
                       {"dstBlendAlpha", state.dstBlendAlpha}, {"blendOpAlpha", state.blendOpAlpha},   {"colorWriteRgb", state.colorWriteRgb},
                       {"colorWriteAlpha", state.colorWriteAlpha}, {"polymodeLine", state.polymodeLine}, {"depthWrite", state.depthWrite},
                       {"depthTest", Nullable(state.depthTest)},         {"polygonOffset", state.polygonOffset}};
            if (state.stencilFront)
                out["stencilFront"] = ToSorted(*state.stencilFront);
            if (state.stencilBack)
                out["stencilBack"] = ToSorted(*state.stencilBack);
            return out;
        }

        T6Texture TextureFromJson(const Json& data)
        {
            T6Texture texture;
            texture.name = Optional<std::string>(data, "name");
            texture.nameStart = Optional<std::string>(data, "nameStart");
            texture.nameEnd = Optional<std::string>(data, "nameEnd");
            texture.nameHash = Optional<std::uint32_t>(data, "nameHash");
            texture.semantic = data.at("semantic").get<std::string>();
            texture.matureContent = data.at("isMatureContent").get<bool>();
            const auto& sampler = data.at("samplerState");
            texture.sampler = {sampler.at("filter").get<std::string>(), sampler.at("mipMap").get<std::string>(), sampler.at("clampU").get<bool>(),
                               sampler.at("clampV").get<bool>(), sampler.at("clampW").get<bool>()};
            texture.image = data.at("image").get<std::string>();
            return texture;
        }

        Sorted ToSorted(const T6Texture& texture)
        {
            Sorted out{{"semantic", texture.semantic},
                       {"isMatureContent", texture.matureContent},
                       {"samplerState",
                        {{"filter", texture.sampler.filter}, {"mipMap", texture.sampler.mipMap}, {"clampU", texture.sampler.clampU},
                         {"clampV", texture.sampler.clampV}, {"clampW", texture.sampler.clampW}}},
                       {"image", texture.image}};
            if (texture.name)
                out["name"] = *texture.name;
            if (texture.nameHash)
                out["nameHash"] = *texture.nameHash;
            if (texture.nameStart)
                out["nameStart"] = *texture.nameStart;
            if (texture.nameEnd)
                out["nameEnd"] = *texture.nameEnd;
            return out;
        }

        T6Constant ConstantFromJson(const Json& data)
        {
            T6Constant constant;
            constant.name = Optional<std::string>(data, "name");
            constant.nameFragment = Optional<std::string>(data, "nameFragment");
            constant.nameHash = Optional<std::uint32_t>(data, "nameHash");
            constant.literal = data.at("literal").get<std::array<double, 4>>();
            return constant;
        }

        Sorted ToSorted(const T6Constant& constant)
        {
            Sorted out{{"literal", constant.literal}};
            if (constant.name)
                out["name"] = *constant.name;
            if (constant.nameFragment)
                out["nameFragment"] = *constant.nameFragment;
            if (constant.nameHash)
                out["nameHash"] = *constant.nameHash;
            return out;
        }

        Sorted ToSorted(const T6Material& material)
        {
            Sorted out{{"$schema", material.schema},
                       {"_game", "t6"},
                       {"_type", "material"},
                       {"_version", 1},
                       {"layeredSurfaceTypes", material.layeredSurfaceTypes},
                       {"surfaceFlags", material.surfaceFlags},
                       {"contents", material.contents},
                       {"gameFlags", material.gameFlags},
                       {"sortKey", material.sortKey},
                       {"surfaceTypeBits", material.surfaceTypeBits},
                       {"stateBitsEntry", material.stateBitsEntry},
                       {"stateFlags", material.stateFlags},
                       {"cameraRegion", material.cameraRegion},
                       {"techniqueSet", material.techniqueSet},
                       {"textures", Sorted::array()},
                       {"constants", Sorted::array()},
                       {"stateBits", Sorted::array()}};
            if (material.thermalMaterial)
                out["thermalMaterial"] = *material.thermalMaterial;
            out["textureAtlas"] = material.textureAtlas ? Sorted{{"rows", material.textureAtlas->rows}, {"columns", material.textureAtlas->columns}}
                                                        : Sorted();
            for (const auto& texture : material.textures)
                out["textures"].push_back(ToSorted(texture));
            for (const auto& constant : material.constants)
                out["constants"].push_back(ToSorted(constant));
            for (const auto& state : material.stateBits)
                out["stateBits"].push_back(ToSorted(state));
            return out;
        }
    } // namespace

    T6StateBits ToT6(const WawStateBits& state)
    {
        const auto nullable = [](const std::string& value) { return value.empty() ? std::nullopt : std::optional(value); };
        const auto stencil = [](const std::optional<WawStencil>& s) -> std::optional<T6Stencil>
        {
            return s ? std::optional(T6Stencil{s->pass, s->fail, s->zfail, s->func}) : std::nullopt;
        };
        return {state.srcBlendRgb,   state.dstBlendRgb,   state.blendOpRgb,      nullable(state.alphaTest), nullable(state.cullFace),
                state.srcBlendAlpha, state.dstBlendAlpha, state.blendOpAlpha,    state.colorWriteRgb,       state.colorWriteAlpha,
                state.polymodeLine,  state.depthWrite,    nullable(state.depthTest), state.polygonOffset,  stencil(state.stencilFront),
                stencil(state.stencilBack)};
    }

    T6Texture ToT6(const WawTexture& texture)
    {
        T6Texture out;
        if (texture.name)
            out.name = texture.name;
        else
        {
            out.nameHash = texture.nameHash;
            out.nameStart = texture.nameStart;
            out.nameEnd = texture.nameEnd;
        }
        out.semantic = texture.semantic;
        out.matureContent = texture.matureContent;
        out.sampler = {texture.sampler.filter, texture.sampler.mipMap, texture.sampler.clampU, texture.sampler.clampV, texture.sampler.clampW};
        out.image = texture.image;
        return out;
    }

    void CopyBlendState(T6StateBits& to, const T6StateBits& from)
    {
        to.srcBlendRgb = from.srcBlendRgb;
        to.dstBlendRgb = from.dstBlendRgb;
        to.blendOpRgb = from.blendOpRgb;
        to.srcBlendAlpha = from.srcBlendAlpha;
        to.dstBlendAlpha = from.dstBlendAlpha;
        to.blendOpAlpha = from.blendOpAlpha;
        to.alphaTest = from.alphaTest;
    }

    void CopyRasterState(T6StateBits& to, const T6StateBits& from)
    {
        to.depthTest = from.depthTest;
        to.depthWrite = from.depthWrite;
        to.polygonOffset = from.polygonOffset;
        to.cullFace = from.cullFace;
        to.colorWriteRgb = from.colorWriteRgb;
        to.colorWriteAlpha = from.colorWriteAlpha;
    }

    bool SameBlendState(const T6StateBits& a, const T6StateBits& b)
    {
        auto copy = a;
        CopyBlendState(copy, b);
        return copy == a;
    }

    bool SameRasterState(const T6StateBits& a, const T6StateBits& b)
    {
        auto copy = a;
        CopyRasterState(copy, b);
        return copy == a;
    }

    T6Texture* T6Material::FindTexture(std::string_view name)
    {
        for (auto& texture : textures)
            if (texture.name == name)
                return &texture;
        return nullptr;
    }

    const T6Texture* T6Material::FindTexture(std::string_view name) const
    {
        return const_cast<T6Material*>(this)->FindTexture(name);
    }

    T6Constant* T6Material::FindConstant(std::string_view name)
    {
        for (auto& constant : constants)
            if (constant.name == name)
                return &constant;
        return nullptr;
    }

    T6Material T6MaterialFromJson(const Json& data)
    {
        if (data.value("_type", std::string()) != "material")
            throw std::runtime_error("not an OpenAssetTools material document");
        T6Material material;
        material.schema = data.value("$schema", material.schema);
        material.layeredSurfaceTypes = data.at("layeredSurfaceTypes").get<unsigned>();
        material.surfaceFlags = data.at("surfaceFlags").get<unsigned>();
        material.contents = data.at("contents").get<unsigned>();
        material.thermalMaterial = Optional<std::string>(data, "thermalMaterial");
        material.gameFlags = data.at("gameFlags").get<std::vector<std::string>>();
        material.sortKey = data.at("sortKey").get<unsigned>();
        if (const auto& atlas = data.at("textureAtlas"); !atlas.is_null())
            material.textureAtlas = T6Atlas{atlas.at("rows").get<unsigned>(), atlas.at("columns").get<unsigned>()};
        material.surfaceTypeBits = data.at("surfaceTypeBits").get<unsigned>();
        material.stateBitsEntry = data.at("stateBitsEntry").get<std::vector<int>>();
        material.stateFlags = data.at("stateFlags").get<unsigned>();
        material.cameraRegion = data.at("cameraRegion").get<std::string>();
        material.techniqueSet = data.at("techniqueSet").get<std::string>();
        for (const auto& texture : data.at("textures"))
            material.textures.push_back(TextureFromJson(texture));
        for (const auto& constant : data.at("constants"))
            material.constants.push_back(ConstantFromJson(constant));
        for (const auto& state : data.at("stateBits"))
            material.stateBits.push_back(StateFromJson(state));
        return material;
    }

    Json ToJson(const T6Material& material)
    {
        return Json(ToSorted(material));
    }

    T6Material ReadT6Material(const std::filesystem::path& path)
    {
        return T6MaterialFromJson(Json::parse(files::ReadText(path)));
    }

    std::string T6MaterialText(const T6Material& material)
    {
        return py::JsonDumps(ToJson(material), 2) + "\n";
    }
} // namespace w2b::materials

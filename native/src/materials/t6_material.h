#pragma once
// A T6 material as the converter builds it: the OpenAssetTools material.v1
// document the BO2 linker reads (materials/<name>.json). Stock BO2 donor
// materials load into it, the converter binds the WaW material's inputs, and
// it is written back in the dumper's own layout (keys sorted, two-space
// indent) so a donor that is not changed stays byte-identical.
//
// Render state values keep the names OpenAssetTools uses ("one", "srcalpha",
// "none", ...), the same names WawStateBits holds, so WaW state copies across
// without a translation table.

#include "core/pytext.h"
#include "materials/waw_material.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace w2b::materials
{
    struct T6Stencil
    {
        std::string pass, fail, zfail, func;
        bool operator==(const T6Stencil&) const = default;
    };

    struct T6StateBits
    {
        std::string srcBlendRgb, dstBlendRgb, blendOpRgb;
        std::optional<std::string> alphaTest, cullFace; // null: a WaW value T6 has no name for
        std::string srcBlendAlpha, dstBlendAlpha, blendOpAlpha;
        bool colorWriteRgb = false, colorWriteAlpha = false, polymodeLine = false, depthWrite = false;
        std::optional<std::string> depthTest;
        std::string polygonOffset;
        std::optional<T6Stencil> stencilFront, stencilBack;
        bool operator==(const T6StateBits&) const = default;
    };

    struct T6Sampler
    {
        std::string filter, mipMap;
        bool clampU = false, clampV = false, clampW = false;
        bool operator==(const T6Sampler&) const = default;
    };

    struct T6Texture
    {
        // the slot name; a stock slot the dump does not name keeps its hash and first and last characters
        std::optional<std::string> name, nameStart, nameEnd;
        std::optional<std::uint32_t> nameHash;
        std::string semantic;
        bool matureContent = false;
        T6Sampler sampler;
        std::string image; // ",name" references an image the zone does not carry
        bool operator==(const T6Texture&) const = default;
    };

    struct T6Constant
    {
        std::optional<std::string> name, nameFragment;
        std::optional<std::uint32_t> nameHash;
        std::array<double, 4> literal{}; // as the file holds them (float values written as doubles)
        bool operator==(const T6Constant&) const = default;
    };

    struct T6Atlas
    {
        unsigned rows = 1, columns = 1;
        bool operator==(const T6Atlas&) const = default;
    };

    struct T6Material
    {
        std::string schema = "http://openassettools.dev/schema/material.v1.json";
        unsigned layeredSurfaceTypes = 0, surfaceFlags = 0, contents = 0;
        std::optional<std::string> thermalMaterial;
        std::vector<std::string> gameFlags; // OAT names them by value: "1", "4", "100", ...
        unsigned sortKey = 0;
        std::optional<T6Atlas> textureAtlas;
        unsigned surfaceTypeBits = 0;
        std::vector<int> stateBitsEntry;    // per technique: an index into stateBits, -1 when unused
        unsigned stateFlags = 0;
        std::string cameraRegion = "none";
        std::string techniqueSet;
        std::vector<T6Texture> textures;
        std::vector<T6Constant> constants;
        std::vector<T6StateBits> stateBits;

        T6Texture* FindTexture(std::string_view name);
        const T6Texture* FindTexture(std::string_view name) const;
        T6Constant* FindConstant(std::string_view name);
        bool operator==(const T6Material&) const = default;
    };

    // WaW render state and textures as a T6 material holds them (an empty WaW value is null)
    T6StateBits ToT6(const WawStateBits& state);
    T6Texture ToT6(const WawTexture& texture);
    // the blend fields and alpha test; the depth, cull, offset and colour-write fields
    void CopyBlendState(T6StateBits& to, const T6StateBits& from);
    void CopyRasterState(T6StateBits& to, const T6StateBits& from);
    bool SameBlendState(const T6StateBits& a, const T6StateBits& b);
    bool SameRasterState(const T6StateBits& a, const T6StateBits& b);

    T6Material T6MaterialFromJson(const py::Json& data);
    py::Json ToJson(const T6Material& material);
    T6Material ReadT6Material(const std::filesystem::path& path);
    // the file text, as OpenAssetTools' dumper lays it out
    std::string T6MaterialText(const T6Material& material);
} // namespace w2b::materials

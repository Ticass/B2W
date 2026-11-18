#pragma once
// A WaW material as the converter reads it: filled straight from a zone
// loaded in process (oat::ReadMaterial), or from the JSON file OpenAssetTools
// dumps for it while some sources still come from dumps (waw_material_json.h).
// Plain types only: w2b_oat fills it next to OAT's own headers.
//
// Render state values keep the names OpenAssetTools gives them ("one",
// "srcalpha", "none", ...), which are also the names T6 material files use,
// so state copied onto a BO2 material needs no translation table.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace w2b::materials
{
    struct WawStencil
    {
        std::string pass, fail, zfail, func;
        bool operator==(const WawStencil&) const = default;
    };

    struct WawStateBits
    {
        std::string srcBlendRgb, dstBlendRgb, blendOpRgb;
        std::string alphaTest; // empty: not a valid WaW value (OAT writes null)
        std::string cullFace;  // empty: not a valid WaW value
        std::string srcBlendAlpha, dstBlendAlpha, blendOpAlpha;
        bool colorWriteRgb = false, colorWriteAlpha = false, polymodeLine = false, depthWrite = false;
        std::string depthTest; // empty: not a valid WaW value
        std::string polygonOffset;
        std::optional<WawStencil> stencilFront, stencilBack;
        bool operator==(const WawStateBits&) const = default;
    };

    struct WawSampler
    {
        std::string filter, mipMap;
        bool clampU = false, clampV = false, clampW = false;
        bool operator==(const WawSampler&) const = default;
    };

    // the simulation of a water map texture
    struct WawWater
    {
        float floatTime = 0;
        int m = 0, n = 0;
        std::string h0, wTerm; // base64, as OAT writes them
        float lx = 0, lz = 0, gravity = 0, windvel = 0;
        std::array<float, 2> winddir{};
        float amplitude = 0;
        std::array<float, 4> codeConstant{};
        bool operator==(const WawWater&) const = default;
    };

    struct WawTexture
    {
        // the slot name; when the zone does not know it only its hash and the
        // first and last characters are known
        std::optional<std::string> name;
        std::optional<std::uint32_t> nameHash;
        std::string nameStart, nameEnd;
        std::string semantic;
        bool matureContent = false;
        WawSampler sampler;
        std::string image; // without the leading ',' of a reference
        std::optional<WawWater> water;
        bool operator==(const WawTexture&) const = default;
    };

    struct WawConstant
    {
        std::optional<std::string> name; // absent: only nameHash and nameFragment are known
        std::string nameFragment;
        std::optional<std::uint32_t> nameHash;
        std::array<float, 4> literal{};
        bool operator==(const WawConstant&) const = default;
    };

    struct WawMaterial
    {
        // a game flag OAT names "4": the NO_MARKS flag of a T6 material
        static constexpr unsigned GAME_FLAG_NO_MARKS = 0x4;
        // the stateBitsEntry index of the unlit technique
        static constexpr std::size_t UNLIT_PASS = 4;

        std::string techniqueSet;
        unsigned gameFlags = 0;
        unsigned sortKey = 0;
        unsigned atlasRows = 0, atlasColumns = 0;
        unsigned surfaceTypeBits = 0;
        std::vector<int> stateBitsEntry;
        unsigned stateFlags = 0;
        std::string cameraRegion;
        std::vector<WawTexture> textures;
        std::vector<WawConstant> constants;
        std::vector<WawStateBits> stateBits;

        // the render state of a technique, if the material has one for it
        const WawStateBits* TechniqueState(std::size_t technique) const;
        bool operator==(const WawMaterial&) const = default;
    };
} // namespace w2b::materials

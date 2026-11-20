// T6 material documents load into T6Material and write back byte for byte.
#include "core/files.h"
#include "materials/t6_material.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace
{
    constexpr std::string_view material = R"({
  "$schema": "http://openassettools.dev/schema/material.v1.json",
  "_game": "t6",
  "_type": "material",
  "_version": 1,
  "cameraRegion": "lit",
  "constants": [
    {
      "literal": [
        1.0,
        0.5,
        0.25,
        1.0
      ],
      "name": "colorTint"
    }
  ],
  "contents": 1,
  "gameFlags": [
    "100"
  ],
  "layeredSurfaceTypes": 536870912,
  "sortKey": 4,
  "stateBits": [
    {
      "alphaTest": "gt0",
      "blendOpAlpha": "add",
      "blendOpRgb": "add",
      "colorWriteAlpha": true,
      "colorWriteRgb": true,
      "cullFace": "back",
      "depthTest": "less_equal",
      "depthWrite": true,
      "dstBlendAlpha": "zero",
      "dstBlendRgb": "zero",
      "polygonOffset": "offset0",
      "polymodeLine": false,
      "srcBlendAlpha": "one",
      "srcBlendRgb": "one",
      "stencilFront": {
        "fail": "keep",
        "func": "always",
        "pass": "replace",
        "zfail": "keep"
      }
    }
  ],
  "stateBitsEntry": [
    0,
    -1
  ],
  "stateFlags": 3,
  "surfaceFlags": 13631488,
  "surfaceTypeBits": 0,
  "techniqueSet": "mc_lit_sm_r0c0n0s0",
  "textureAtlas": {
    "columns": 1,
    "rows": 1
  },
  "textures": [
    {
      "image": "~-gcamo_bark",
      "isMatureContent": false,
      "name": "colorMap",
      "samplerState": {
        "clampU": false,
        "clampV": false,
        "clampW": false,
        "filter": "aniso2x",
        "mipMap": "linear"
      },
      "semantic": "colorMap"
    },
    {
      "image": ",$identitynormalmap",
      "isMatureContent": false,
      "nameEnd": "p",
      "nameHash": 1503017986,
      "nameStart": "n",
      "samplerState": {
        "clampU": false,
        "clampV": false,
        "clampW": false,
        "filter": "aniso2x",
        "mipMap": "linear"
      },
      "semantic": "normalMap"
    }
  ]
}
)";
}

TEST_CASE("T6 material documents round-trip byte for byte", "[materials]")
{
    using namespace w2b::materials;
    const auto parsed = T6MaterialFromJson(w2b::util::Json::parse(material));
    CHECK(T6MaterialText(parsed) == material);
    REQUIRE(parsed.FindTexture("colorMap"));
    CHECK(parsed.FindTexture("colorMap")->image == "~-gcamo_bark");
    CHECK(parsed.textures[1].nameHash == 1503017986u);
    CHECK(parsed.stateBits[0].stencilFront->pass == "replace");

    // W2B_MATERIAL_ROUNDTRIP=<folder>: every material document under it
    if (const char* folder = std::getenv("W2B_MATERIAL_ROUNDTRIP"); folder && *folder)
        for (const auto& path : w2b::files::RGlob(w2b::files::FromUtf8(folder), ".json"))
        {
            const auto text = w2b::files::ReadText(path);
            INFO(w2b::files::Utf8(path));
            CHECK(T6MaterialText(ReadT6Material(path)) == text);
        }
}

#include "shaders/techsets.h"

TEST_CASE("T6 technique sets round-trip", "[materials]")
{
    // W2B_TECHSET_ROUNDTRIP=<folder>: every techniquesets document under it
    const char* folder = std::getenv("W2B_TECHSET_ROUNDTRIP");
    if (!folder || !*folder)
        return;
    for (const auto& path : w2b::files::RGlob(w2b::files::FromUtf8(folder), ".json"))
    {
        if (w2b::files::Utf8(path).find("techniquesets") == std::string::npos || w2b::files::Utf8(path).find("waw_techniquesets") != std::string::npos)
            continue;
        INFO(w2b::files::Utf8(path));
        const auto data = w2b::util::Json::parse(w2b::files::ReadText(path));
        const auto written = w2b::shaders::ToJson(w2b::shaders::T6TechniqueSetFromJson(data));
        // key order aside: an argument the runtime added type-first is written sorted
        CHECK(w2b::util::Json::diff(data, written).empty());
    }
}

TEST_CASE("JSON documents match by content", "[parity]")
{
    // W2B_JSON_PAIRS=<file>: lines "<base>|<new>"; each pair must hold the same document, key order aside
    const char* list = std::getenv("W2B_JSON_PAIRS");
    if (!list || !*list)
        return;
    const auto pairs = w2b::files::ReadText(w2b::files::FromUtf8(list)); // SplitLines views into it
    for (const auto line : w2b::util::SplitLines(pairs))
    {
        const auto bar = line.find('|');
        if (bar == std::string_view::npos)
            continue;
        const auto base = w2b::files::FromUtf8(std::string(line.substr(0, bar)));
        const auto fresh = w2b::files::FromUtf8(std::string(line.substr(bar + 1)));
        INFO(std::string(line.substr(bar + 1)));
        const auto a = nlohmann::json::parse(w2b::files::ReadText(base));
        const auto b = nlohmann::json::parse(w2b::files::ReadText(fresh));
        INFO((a == b ? std::string() : nlohmann::json::diff(a, b).dump().substr(0, 600)));
        CHECK(a == b);
    }
}

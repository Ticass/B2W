#include "shaders/d3d.h"
#include "shaders/translate.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

using namespace w2b;

TEST_CASE("Translate lowers a small pixel program")
{
    const auto result = shaders::Translate("ps_3_0\n"
                                           "def c1, 0.5, 1, 0, 2\n"
                                           "dcl_texcoord v0.xy\n"
                                           "dcl_2d s0\n"
                                           "texld r0, v0, s0\n"
                                           "mad_sat oC0, r0, c0, c1\n",
                                           nullptr);
    // the expected translation of this input
    CHECK(result.hlsl
          == "cbuffer WawBuffer0 : register(b0) { float4 t6_cb0[1]; };\n"
             "static const float4 c1 = float4(0.5, 1.0, 0.0, 2.0);\n"
             "Texture2D<float4> tex_s0 : register(t0);\n"
             "SamplerState samp_s0 : register(s0);\n"
             "struct Input { float2 v0 : TEXCOORD0; };\n"
             "struct Output { float4 oC0 : SV_Target0; };\n"
             "Output main(Input input) {\n"
             "float4 oC0 = 0;\n"
             "float4 r0 = 0;\n"
             "float4 v0 = 0;\n"
             "v0 = float4(input.v0, 0, 0);\n"
             "{ float4 value = tex_s0.Sample(samp_s0, (v0).xy); r0.xyzw = value.xyzw; }\n"
             "{ float4 value = saturate((r0*t6_cb0[0]+c1)); oC0.xyzw = value.xyzw; }\n"
             "Output output;\n"
             "output.oC0 = oC0.xyzw;\n"
             "return output;\n"
             "}\n");
    CHECK(result.report["constants"] == util::Json::array({"c0"}));
    CHECK(result.report["outputs"]["oC0"] == util::Json::array({"SV_Target0", 4}));
}

TEST_CASE("Translate errors carry their messages")
{
    CHECK_THROWS_WITH(shaders::Translate("vs_2_0\n", nullptr), "requires ps_3_0 or vs_3_0 bytecode");
    CHECK_THROWS_WITH(shaders::Translate("ps_3_0\nmov oC0, r9.q\n", nullptr), "unsupported operand r9.q");
    CHECK_THROWS_WITH(shaders::Translate("ps_3_0\nfoo oC0, r0\n", nullptr), "unsupported instruction: foo oC0, r0");
}

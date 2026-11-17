#include "shaders/registers.h"

#include <catch2/catch_test_macros.hpp>

using namespace w2b::shaders;

TEST_CASE("MatchRegister follows REG.fullmatch")
{
    Register r;
    CHECK(MatchRegister("r12", r));
    CHECK(r.reg == "r12");
    CHECK_FALSE(r.hasSwizzle);
    CHECK(MatchRegister("oC0.xyzw", r));
    CHECK(r.reg == "oC0");
    CHECK(r.swizzle == "xyzw");
    CHECK(MatchRegister("oDepth", r));
    CHECK(MatchRegister("o3.rg", r));
    CHECK(r.reg == "o3");
    CHECK_FALSE(MatchRegister("oC", r));
    CHECK_FALSE(MatchRegister("r", r));
    CHECK_FALSE(MatchRegister("r1.xyzwx", r));
    CHECK_FALSE(MatchRegister("r1.q", r));
    CHECK_FALSE(MatchRegister("s0", r));
    CHECK_FALSE(MatchRegister("-r0", r));
}

TEST_CASE("FindRegisters follows the findall pattern")
{
    CHECK(FindRegisters("r0, v1.xy, c12") == std::vector<std::string>{"r0", "v1", "c12"});
    CHECK(FindRegisters("oC0, r1_abs.x") == std::vector<std::string>{"oC0", "r1"});
    CHECK(FindRegisters("r0x, br1, -r2") == std::vector<std::string>{"r2"});
    CHECK(FindRegisters("abs(r3)") == std::vector<std::string>{"r3"});
    CHECK(FindRegisters("oDepth, oDepthx") == std::vector<std::string>{"oDepth"});
}

TEST_CASE("modifiers, semantics and swizzles")
{
    std::string base, swizzle;
    CHECK(MatchAbsModifier("r1_abs.x", base, swizzle));
    CHECK(base == "r1");
    CHECK(swizzle == ".x");
    CHECK_FALSE(MatchAbsModifier("o1_abs", base, swizzle));
    CHECK(MatchSemantic("TEXCOORD0"));
    CHECK_FALSE(MatchSemantic("0TEX"));
    CHECK_FALSE(MatchSemantic("Tex"));
    CHECK(SwizzleXyzw("rgba") == "xyzw");
}

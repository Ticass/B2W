#include "world/vertices.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <sstream>
#include <stdexcept>

TEST_CASE("World vertices retain Python's packed color and normal semantics")
{
    constexpr auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    constexpr std::array packed{bits(1.25f), bits(-2.0f), bits(3.0f), 0x80302010u,
                                bits(0.5f), bits(0.75f), bits(0.25f), bits(1.0f),
                                0x8157e056u, 0x007f7f7fu, bits(-1.0f)};
    constexpr std::array legacy{-0x1.74b1cbcbd25fdp-2, 0x1.b8decebc0877ap-1, -0x1.6b9aba5675d42p-2};
    constexpr std::array compensated{-0x1.74b1cbcbd25ffp-2, 0x1.b8decebc0877bp-1, -0x1.6b9aba5675d43p-2};
    for (bool useCompensation : {false, true})
    {
        std::istringstream input(std::string(reinterpret_cast<const char*>(packed.data()), sizeof(packed)));
        std::stringstream output;
        w2b::world::DecodeVertices(input, output, 1, useCompensation);
        const auto& normal = useCompensation ? compensated : legacy;
        const std::array expected{1.25, -2.0, 3.0, 48.0 / 255.0, 32.0 / 255.0, 16.0 / 255.0, 128.0 / 255.0,
                                  0.5, 0.75, 0.25, 1.0, normal[0], normal[1], normal[2], 0.0, 0.0, 1.0, -1.0};
        std::array<double, 18> actual;
        REQUIRE(output.read(reinterpret_cast<char*>(actual.data()), sizeof(actual)).good());
        CHECK(actual == expected);
    }
}

TEST_CASE("World vertex decoding rejects a partial record")
{
    std::istringstream input(std::string(43, '\0'));
    std::ostringstream output;
    CHECK_THROWS_AS(w2b::world::DecodeVertices(input, output, 1, true), std::runtime_error);
    CHECK(output.str().empty());
}

#include "world/fbx_writer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>

TEST_CASE("World FBX compacts sparse vertices and exports layer data in first-use order")
{
    constexpr w2b::world::Vertex sample{{1, 2, 3}, {0.1, 0.2, 0.3, 0.4}, {0.25, 0.75},
                                        {0.5, 0.75}, {0, 0, 1}, {1, 0, 0}, -1};
    std::array vertices{sample, sample, sample, sample};
    vertices[3].xyz[0] = 1e-7;
    constexpr std::array<std::uint16_t, 6> indices{3, 0, 2, 0, 2, 3};
    std::array<std::uint8_t, 48> layers{};
    constexpr std::array<float, 2> uv{0.25f, 0.75f};
    std::memcpy(layers.data() + 36, uv.data(), sizeof(uv));
    layers[44] = 1;
    layers[45] = 2;
    layers[46] = 3;
    layers[47] = 4;
    const w2b::world::RenderBuffers buffers{vertices, indices, layers};
    const std::array meshes{w2b::world::FbxMesh{"mc/a", 2, 3, 4, 5, true, true, {{0, 0, 2, 0, 2}}}};
    const auto geometry = w2b::world::BuildFbxGeometry(buffers, meshes.front());
    CHECK(geometry.polygons == std::vector<int>{0, 2, -2, 1, 0, -3});
    CHECK(geometry.colors == std::vector<double>(12, 1.0));
    REQUIRE(geometry.layers.size() == 2);
    CHECK(geometry.layers[0].name == "LayerUV1");
    CHECK(geometry.layers[0].values == std::vector<double>{0.25, 0.25, 0, 1, 0, 1});
    CHECK(geometry.layers[1].name == "LayerNormal1");
    CHECK(geometry.layers[1].values == std::vector<double>{513, 1027, 0, 0, 0, 0});

    std::ostringstream out;
    w2b::world::WriteWorldFbx(out, buffers, meshes);
    CHECK(out.str().find("Geometry::waw_mesh_00000_nd1_bm2_pl3_rp4_lm5") != std::string::npos);
    CHECK(out.str().find("Vertices: *9 { a: 1e-07,3,-2,1,3,-2,1,3,-2 }") != std::string::npos);
    CHECK(out.str().find(" C: \"OO\",2000000,10001\n") != std::string::npos);
}

TEST_CASE("World FBX rejects an out-of-range triangle")
{
    constexpr std::array<std::uint16_t, 1> indices{0};
    const w2b::world::FbxMesh mesh{"mc/a", 0, 0, 0, 0, false, false, {{0, 0, 1, 0, 0}}};
    CHECK_THROWS_AS(w2b::world::BuildFbxGeometry({{}, indices, {}}, mesh), std::runtime_error);
}

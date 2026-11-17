#include "world/collision_plan.h"
#include "world/terrain_ownership.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <random>
#include <stdexcept>
#include <string_view>

namespace
{
    using namespace w2b::world;

    class Fixture
    {
    public:
        Fixture() : path(std::filesystem::temp_directory_path() /
                         ("w2b-clip-" + std::to_string(std::random_device{}()) + ".bin"))
        {
        }

        ~Fixture()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }

        std::filesystem::path path;
    };

    template <typename T>
    void Write(std::ostream& out, const T& value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    void String(std::ostream& out, std::string_view text)
    {
        Write(out, static_cast<std::uint32_t>(text.size()));
        out.write(text.data(), text.size());
    }

    void WriteClip(const std::filesystem::path& path, std::uint32_t version)
    {
        std::ofstream out(path, std::ios::binary);
        out.write("W2BSP001", 8);
        Write(out, version);
        String(out, "maps/test");
        Write(out, 1u); // materials
        String(out, "clip");
        Write(out, 0x80000000u);
        Write(out, 1u);
        Write(out, 1u); // vertices
        Write(out, ClipPoint{1, 2, 3});
        Write(out, 1u); // triangles
        Write(out, std::array<std::uint16_t, 3>{0, 0, 0});
        Write(out, 1u); // brushes
        Write(out, ClipPoint{});
        Write(out, ClipPoint{1, 2, 3});
        Write(out, 1u); // contents
        Write(out, 1u); // planes
        Write(out, BrushPlane{{0, 0, 1}, 3, 0});
        Write(out, 0u); // brush vertices
        if (version >= 3)
        {
            Write(out, 1u); // cmodels
            Write(out, ClipPoint{});
            Write(out, ClipPoint{1, 2, 3});
            Write(out, 1u); // brush ids
            Write(out, std::uint16_t{0});
            Write(out, 0u); // static models
        }
        if (version >= 4)
        {
            Write(out, 1u); // triangle materials
            Write(out, std::uint16_t{0});
        }
        if (version >= 6)
        {
            Write(out, 4u); // compiled edge bitset bytes
            Write(out, 3u);
        }
        if (version >= 7)
        {
            Write(out, 1u); // terrain nodes
            Write(out, TerrainNode{0, 0, 0});
            Write(out, 1u); // terrain roots
            Write(out, TerrainRoot{0, 1, 1});
            Write(out, 1u); // partitions
            Write(out, TerrainPartition{0, 1});
        }
        if (version >= 8)
        {
            Write(out, 1u); // brush reach
            Write(out, 1u);
        }
        if (version >= 9)
        {
            Write(out, 1u); // model roots
            Write(out, TerrainRoot{0, 0, 0});
        }
    }
}

TEST_CASE("Native clip reader preserves versions 2 through 9")
{
    const Fixture fixture;
    for (std::uint32_t version = 2; version <= 9; ++version)
    {
        CAPTURE(version);
        WriteClip(fixture.path, version);
        const auto clip = ReadCollisionWorld(fixture.path);
        CHECK(clip.name == "maps/test");
        CHECK(clip.materials.at(0).surfaceFlags == 0x80000000u);
        CHECK(clip.vertices.at(0) == ClipPoint{1, 2, 3});
        CHECK(clip.brushes.at(0).planes.at(0).distance == 3);
        CHECK(clip.models.empty() == (version < 3));
        CHECK(clip.triangleMaterials.empty() == (version < 4));
        CHECK(clip.brushOwnershipComplete == (version >= 5));
        CHECK(clip.edgeWalkable.empty() == (version < 6));
        CHECK(clip.partitions.empty() == (version < 7));
        CHECK(clip.brushReach.has_value() == (version >= 8));
        CHECK(clip.modelRoots.has_value() == (version >= 9));
    }
    std::filesystem::resize_file(fixture.path, std::filesystem::file_size(fixture.path) - 1);
    CHECK_THROWS_AS(ReadCollisionWorld(fixture.path), std::runtime_error);
}

TEST_CASE("Collision planning keeps moving water and removes one-sided traversal sheets")
{
    CollisionWorld clip;
    clip.vertices = {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    clip.indices = {0, 1, 2, 0, 2, 1, 0, 1, 2};
    clip.materials = {{"clip_water", 0, 0x20}, {"sheet", 0, 0x30200}};
    clip.triangleMaterials = {0, 0, 1};
    clip.terrainNodes = {{0, 0, 0}};
    clip.partitions = {{1, 1}};
    clip.modelRoots = std::vector<TerrainRoot>{{0, 0, 0}, {0, 1, 1}};
    const auto plan = PlanCollision(clip);
    CHECK(plan.triangleSlots == std::vector<int>{-1, 0, -1});
    CHECK(plan.waterDropped == 1);
    CHECK(plan.oneWayTriangles == std::vector<std::size_t>{2});
    REQUIRE(plan.slots.size() == 1);
    CHECK(plan.slots.front().model == 1);

    clip.triangleMaterials[1] = 1;
    CHECK(PlanCollision(clip).oneWayTriangles.empty());
    clip.modelRoots->push_back({0, 1, 1});
    CHECK_THROWS_AS(TerrainOwners(clip), std::runtime_error);
    clip.terrainNodes[0].children = 1;
    CHECK_THROWS_AS(TerrainOwners(clip), std::runtime_error);
}

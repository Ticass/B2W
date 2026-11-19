#include "core/files.h"
#include "pipeline/bridge_images.h"
#include "textures/iwi.h"
#include "textures/iwd_recovery.h"
#include "textures/wavelet.h"
#include "world/probe_plan.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>

namespace
{
    void U32(std::string& bytes, std::size_t start, unsigned value)
    {
        for (unsigned byte = 0; byte < 4; ++byte)
            bytes.at(start + byte) = static_cast<char>(value >> (byte * 8));
    }

    std::string DdsHeader(unsigned width, unsigned height, unsigned levels)
    {
        std::string dds(128, '\0');
        dds.replace(0, 4, "DDS ");
        U32(dds, 4, 124);
        U32(dds, 12, height);
        U32(dds, 16, width);
        U32(dds, 28, levels);
        U32(dds, 76, 32);
        return dds;
    }
}

TEST_CASE("IWI conversion swizzles BGR and expands it to RGBA")
{
    using namespace w2b::textures;
    auto dds = DdsHeader(2, 1, 1);
    U32(dds, 80, 0x40);
    U32(dds, 88, 24);
    U32(dds, 92, 0x00ff0000);
    U32(dds, 96, 0x0000ff00);
    U32(dds, 100, 0x000000ff);
    dds.append("\x01\x02\x03\x04\x05\x06", 6);
    const auto iwi = DdsToIwi(dds);
    CHECK(iwi.header.format == Format::rgba);
    CHECK(iwi.header.flags == noMipmaps);
    CHECK(iwi.header.fileSizes[0] == 72);
    CHECK(iwi.bytes.substr(64) == std::string("\x03\x02\x01\xff\x06\x05\x04\xff", 8));
    dds.pop_back();
    CHECK_THROWS_AS(DdsToIwi(dds), TextureError);
}

TEST_CASE("Face-major DDS cubes keep each face inside its mip")
{
    using namespace w2b::textures;
    auto dds = DdsHeader(2, 2, 2);
    U32(dds, 80, 4);
    dds.replace(84, 4, "DXT1");
    U32(dds, 112, 0x200);
    std::string base, small;
    for (char face = 0; face < 6; ++face)
    {
        const std::string a(8, face), b(8, static_cast<char>(face + 10));
        dds += a + b;
        base += a;
        small += b;
    }
    const auto full = DdsToIwi(dds, true);
    CHECK(full.header.flags == cubemap);
    CHECK(full.header.fileSizes[0] == 160);
    CHECK(full.header.fileSizes[1] == 112);
    CHECK(full.bytes.substr(64) == small + base);
    const auto single = DdsToIwi(dds, true, true);
    CHECK(single.header.flags == (cubemap | noMipmaps));
    CHECK(single.bytes.substr(64) == base);
}

TEST_CASE("Bridge image staging distinguishes missing pixels from an unsearched IWD")
{
    namespace fs = std::filesystem;
    using w2b::py::Json;
    struct Fixture
    {
        fs::path root = fs::temp_directory_path() / ("w2b-bridge-images-" + std::to_string(std::random_device{}()));
        bool owned = fs::create_directory(root);
        ~Fixture()
        {
            if (!owned)
                return;
            std::error_code error;
            for (const auto* path : {"images/$outdoor.iwi", "images/grey.iwi", "images/lightmap0_secondary.iwi",
                                     "images/reflection_probe0.iwi", "images/~$white-rgb&missing.iwi", "images"})
                fs::remove(root / path, error);
            fs::remove(root, error);
        }
    } fixture;
    REQUIRE(fixture.owned);
    const auto& root = fixture.root;
    w2b::textures::IwdRecovery recovery({}, root / "recovered", std::nullopt, {}, {});
    w2b::pipeline::BridgeImagesRequest request{root, {}, std::vector<fs::path>{}, {"grey", "~$white-rgb&missing"}, {}, {}, Json::object(), false, false, 1};
    const auto invented = w2b::pipeline::StageBridgeImages(request, recovery);
    CHECK(invented.written == std::vector<std::string>{"$outdoor", "grey", "lightmap0_secondary", "reflection_probe0", "~$white-rgb&missing"});
    CHECK(invented.errors.empty());
    std::string cube;
    REQUIRE(w2b::files::ReadBytes(root / "images/reflection_probe0.iwi", cube));
    REQUIRE(cube.size() == 64 + 4 * 4 * 4 * 6);
    CHECK(static_cast<unsigned char>(cube[5]) == (w2b::textures::noMipmaps | w2b::textures::cubemap));
    CHECK(cube.substr(64, 4) == std::string("\0\0\0\xff", 4));
    std::string pages;
    REQUIRE(w2b::files::ReadBytes(root / "images/lightmap0_secondary.iwi", pages));
    REQUIRE(pages.size() == 64 + 4 * 12 * 4);
    CHECK(pages.substr(64 + 4 * 4 * 4, 4) == std::string("\x84\xac\xbf\x6f", 4));

    request.iwdDirs.reset();
    const auto unknown = w2b::pipeline::StageBridgeImages(request, recovery);
    CHECK(unknown.errors == std::vector<std::string>{"image $outdoor: source $outdoor.dds missing from every image root",
                                                     "image grey: source grey.dds missing from every image root"});
    CHECK(unknown.written == std::vector<std::string>{"lightmap0_secondary", "reflection_probe0", "~$white-rgb&missing"});
}

TEST_CASE("Alpha wavelet cubemaps retain all six faces at both mip levels")
{
    using namespace w2b::textures;
    WaveletTables tables;
    for (auto& book : tables)
        book.fill({0, 1});
    std::string iwi(28, '\0');
    iwi.replace(0, 4, "IWi\x06");
    iwi[4] = 10;
    iwi[5] = cubemap;
    iwi[6] = iwi[8] = 2;
    iwi[10] = 1;
    std::string fullMip, smallMip;
    for (char sample = 40; sample < 46; ++sample)
    {
        smallMip += sample;
        fullMip.append(4, sample);
    }
    iwi += smallMip;
    iwi.append(4, '\0'); // Six faces of five bits each.
    const auto dds = WaveletToDds(iwi, tables);
    CHECK(dds.substr(128) == fullMip + smallMip);
    CHECK(static_cast<unsigned char>(dds[80]) == 0x2);
    CHECK(static_cast<unsigned char>(dds[113]) == 0xfe);
    iwi.pop_back();
    CHECK_THROWS_AS(WaveletToDds(iwi, tables), TextureError);
}

TEST_CASE("Over-budget reflection probes preserve the deterministic WaW-to-T6 remap")
{
    std::vector<std::array<double, 3>> origins{{0, 0, 0}};
    std::vector<int> baked;
    for (int i = 1; i <= 40; ++i)
    {
        origins.push_back({8.0 * i, 0, 0});
        baked.push_back(i);
    }
    const auto plan = w2b::world::PlanProbes(origins, {{8, 5}, {18, 3}}, baked, true);
    constexpr std::array tail{29, 31, 34, 37, 39};
    REQUIRE(plan.kept.size() == 32);
    for (int i = 0; i <= 26; ++i)
        CHECK(plan.kept[i] == i);
    for (std::size_t i = 0; i < tail.size(); ++i)
        CHECK(plan.kept[27 + i] == tail[i]);
    CHECK(plan.Target(40) == 31);
    CHECK(plan.Target(100) == 0);
}

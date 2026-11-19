// build-mod helpers against CPython's csv module and modzone.py.
#include "core/files.h"
#include "pipeline/actor_trees.h"
#include "pipeline/bridge_inventory.h"
#include "pipeline/csvtable.h"
#include "pipeline/geometry_materials.h"
#include "pipeline/modzone.h"
#include "world/render_world.h"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace w2b::pipeline;

TEST_CASE("Bridge inventory preserves weapon include matching and source names", "[pipeline]")
{
    namespace fs = std::filesystem;
    using w2b::files::WriteText;
    using w2b::py::Json;
    struct Fixture
    {
        fs::path root = fs::temp_directory_path() / ("w2b-inventory-" + std::to_string(std::random_device{}()));
        bool created = fs::create_directory(root);
        ~Fixture()
        {
            if (!created)
                return;
            std::error_code error;
            for (const auto* name : {"entities.json", "maps/source.gsc", "maps/utf8.gsc", "weapons/compiled", "soundaliases/hit.w2bsnd.json",
                                     "xmodel/sub/tree.json", "maps", "weapons", "soundaliases", "xmodel/sub", "xmodel"})
                fs::remove(root / name, error);
            fs::remove(root, error);
        }
    } fixture;
    const auto& root = fixture.root;
    REQUIRE(fixture.created);
    WriteText(root / "entities.json", R"({"entities":[{"classname":"weapon_world"},{"classname":"misc_turret","weaponinfo":"turret"}]})");
    WriteText(root / "maps/source.gsc", "include_weapon(\"\"); xinclude_weapon(\"ignored\"); INCLUDE_WEAPON \n(\t\"mixed\");\n");
    WriteText(root / "maps/utf8.gsc", "\xC4\xB0NCLUDE_WEAPON(\"unicode\");");
    WriteText(root / "weapons/compiled", "WEAPONFILE\\data");
    WriteText(root / "soundaliases/hit.w2bsnd.json", "{}");
    WriteText(root / "xmodel/sub/tree.json", "{}");

    auto warnings = Json::array();
    const auto inventory = BridgeInventory({root}, {root}, root / "entities.json", {"ignored", "mixed", "unicode", "turret", "world"}, warnings);
    CHECK(inventory.at("compiled_weapons") == Json::array({"compiled", "mixed", "turret", "unicode", "world"}));
    CHECK(inventory.at("zone_sounds") == Json::array({"hit"}));
    CHECK(inventory.at("shipped_models") == Json::array({"sub/tree"}));
    CHECK(warnings.size() == 4);
}

TEST_CASE("Bridge geometry resolves vertex layers by stride and generated layer count", "[pipeline]")
{
    w2b::world::RenderWorld world;
    world.layerData.resize(57);
    const auto surface = [](int index, std::string name, int offset)
    {
        w2b::world::WorldSurface result{};
        result.index = index;
        result.material = std::move(name);
        result.vertexCount = 1;
        result.layerOffset = offset;
        result.layerFormat = 0xff;
        return result;
    };
    world.surfaces = {surface(0, "inferred", 0), surface(1, "*20_21_22", 8),
                      surface(2, "*30_31_32_33", 32), surface(3, "unresolved", 56)};
    auto warnings = w2b::py::Json::array();
    const auto materials = PlanGeometryMaterials(world, {}, nullptr, warnings);
    CHECK(world.surfaces[0].layerFormat == 1);
    CHECK(world.surfaces[1].layerFormat == 5);
    CHECK(world.surfaces[2].layerFormat == 6);
    CHECK(world.surfaces[3].layerFormat == 0xff);
    CHECK(materials.blendWeights.empty());
    CHECK(warnings == w2b::py::Json::array({"world vertex format unknown for ['unresolved']; layers dropped"}));
}

TEST_CASE("csv.writer QUOTE_MINIMAL", "[pipeline]")
{
    // csv.writer(s, lineterminator="\n").writerows(rows)
    const CsvRows rows{{"a", "b,c", "d\"e", ""}, {""}, {}, {"x\ny", "  sp"}};
    CHECK(CsvText(rows, "\n") == "a,\"b,c\",\"d\"\"e\",\n\"\"\n\n\"x\ny\",  sp\n");
}

TEST_CASE("csv.reader default dialect", "[pipeline]")
{
    // list(csv.reader(io.StringIO(text)))
    const auto rows = ReadCsv("a,\"b,\"\"c\"\nx,\"y\nz\"q\n\n,\n");
    const CsvRows expected{{"a", "b,\"c"}, {"x", "y\nzq"}, {}, {"", ""}};
    CHECK(rows == expected);
    CHECK(ReadCsv("a,b\r\nc") == CsvRows{{"a", "b"}, {"c"}});
}

TEST_CASE("map name key", "[pipeline]")
{
    CHECK(MapNameKey("zm_cc2_waw") == "WAW_ZM_CC2_WAW");
    // over 31 characters with _CAPS: sha1(project)[:16].upper()
    CHECK(MapNameKey("zm_a_very_long_custom_map_name_waw") == "WAW_64B96584E09A8AAE");
}

TEST_CASE("unavailable weapons pair upgrades", "[pipeline]")
{
    const auto names = UnavailableWeapons({"weapon,mp/ray_gun_zm  <- Could not load", "xmodel,foo  <- x", "weapon,tesla_upgraded_zm  <- y"});
    CHECK(names == std::set<std::string>{"ray_gun_zm", "ray_gun_upgraded_zm", "tesla_upgraded_zm", "tesla_zm"});
}

TEST_CASE("mod ipak range opens after the header", "[pipeline]")
{
    CHECK(OpenModIpakFirst(">game,T6\n// c\nweapon,a\n", {">ipak,pack"}) == ">game,T6\n// c\n>level.ipak_read,pack\n>ipak,pack\nweapon,a\n");
    CHECK(OpenModIpakFirst("x\n", {}) == "x\n");
}

TEST_CASE("native tree animations skip groups and comments", "[pipeline]")
{
    // Group names open a block; comments and repeats are dropped; order is kept.
    const std::string tree = "// header comment\nbody\n{\n\tzombie_walk // trailing\n\tzombie_run\n\tzombie_walk\n}\nlegs\n  {\n\tdog_idle\n}\n";
    CHECK(NativeTreeAnimations(tree) == std::vector<std::string>{"zombie_walk", "zombie_run", "dog_idle"});
}

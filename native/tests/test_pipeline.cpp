// build-mod helpers against expected csv parses and mod zones.
#include "core/files.h"
#include "pipeline/actor_trees.h"
#include "pipeline/bridge_inventory.h"
#include "pipeline/csvtable.h"
#include "pipeline/geometry_materials.h"
#include "pipeline/modzone.h"
#include "gsc/port/rewrites.h"
#include "gsc/port/zombiebehavior.h"
#include "gsc/tokens.h"
#include "models/models.h"
#include "visions/visions.h"
#include "weapons/skeleton.h"
#include "weapons/weapon.h"
#include "world/grid_stage.h"
#include "world/light_grid.h"
#include "world/render_world.h"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace w2b::pipeline;

TEST_CASE("Bridge inventory preserves weapon include matching and source names", "[pipeline]")
{
    namespace fs = std::filesystem;
    using w2b::files::WriteText;
    using w2b::util::Json;
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

    std::vector<std::string> warnings;
    const auto inventory = BridgeInventory({root}, {root}, root / "entities.json", {"ignored", "mixed", "unicode", "turret", "world"}, warnings);
    CHECK(inventory.compiledWeapons == std::set<std::string>{"compiled", "mixed", "turret", "unicode", "world"});
    CHECK(inventory.zoneSounds == std::set<std::string>{"hit"});
    CHECK(inventory.shippedModels == std::set<std::string>{"sub/tree"});
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
    auto warnings = w2b::util::Json::array();
    const auto materials = PlanGeometryMaterials(world, {}, nullptr, warnings);
    CHECK(world.surfaces[0].layerFormat == 1);
    CHECK(world.surfaces[1].layerFormat == 5);
    CHECK(world.surfaces[2].layerFormat == 6);
    CHECK(world.surfaces[3].layerFormat == 0xff);
    CHECK(materials.blendWeights.empty());
    CHECK(warnings == w2b::util::Json::array({"world vertex format unknown for ['unresolved']; layers dropped"}));
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

TEST_CASE("weapon numbers read as WaW's atoi/atof", "[pipeline]")
{
    w2b::weapons::Schema schema{{"meleeDelay", "CSPFT_MILLISECONDS"}, {"clipSize", "CSPFT_INT"}, {"damage", "CSPFT_FLOAT"}};
    w2b::weapons::InfoFields source;
    source.fields = {{"meleeDelay", "0..05"}, {"clipSize", "12 rounds"}, {"damage", "0.5"}};
    const auto weapon = w2b::weapons::Convert("w", source, schema, schema);
    CHECK(*weapon.fields.Find("meleeDelay") == "0");
    CHECK(*weapon.fields.Find("clipSize") == "12");
    CHECK(*weapon.fields.Find("damage") == "0.5");
    CHECK(weapon.translations.contains("meleeDelay"));
    CHECK_FALSE(weapon.translations.contains("damage"));
}

TEST_CASE("vision values scan as WaW's sscanf", "[pipeline]")
{
    using w2b::visions::ScanFloats;
    CHECK(ScanFloats("1 1.1 .9", 3) == std::vector<double>{1, 1.1, .9});
    CHECK(ScanFloats("1 1 1 1", 3) == std::vector<double>{1, 1, 1});
    CHECK(ScanFloats("1.0.5 2", 3) == std::vector<double>{1, .5, 2});
    CHECK(ScanFloats("0.O458", 1) == std::vector<double>{0});
    CHECK(ScanFloats("1 1", 3).empty());
    CHECK(ScanFloats("1, 1, 1", 3).empty());
    CHECK(ScanFloats("", 1).empty());
}

TEST_CASE("a script ends at its NUL", "[pipeline]")
{
    const std::string text("main()\n{\n}\n\0\0garbage", 20);
    CHECK(w2b::gsc::Emit(w2b::gsc::Tokenize(text)) == "main()\n{\n}\n");
}

TEST_CASE("an animated model keeps several root bones", "[pipeline]")
{
    const auto model = [] {
        return w2b::util::Json{{"nodes", {{{"name", "tag_weapon"}}, {{"name", "j_gun"}, {"children", {2}}}, {{"name", "tag_flash"}}}},
                               {"skins", {{{"joints", {0, 1, 2}}}}},
                               {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}, {"JOINTS_0", 1}, {"WEIGHTS_0", 2}}}}}}}}}};
    };
    auto animated = model(), rigid = model();
    CHECK_FALSE(w2b::models::StripBadSkins(animated, false));
    CHECK(animated == model());
    CHECK(w2b::models::StripBadSkins(rigid, true));
    CHECK_FALSE(rigid.contains("skins"));
    CHECK(w2b::weapons::PrepareSkeleton(animated, false).empty());
    CHECK(animated == model());
}

TEST_CASE("a non-finite hitbox sphere becomes its box's", "[pipeline]")
{
    w2b::util::Json model{{"boneInfo",
                           {{{"bone", "a"}, {"mins", {-1, -2, -2}}, {"maxs", {1, 2, 2}}, {"offset", {0, 0, 0}}, {"radiusSquared", nullptr}},
                            {{"bone", "b"}, {"mins", {nullptr, 0, 0}}, {"maxs", {1, 1, 1}}, {"offset", {0, 0, 0}}, {"radiusSquared", 4.0}},
                            {{"bone", "c"}, {"mins", {0, 0, 0}}, {"maxs", {1, 1, 1}}, {"offset", {0, 0, 0}}, {"radiusSquared", 3.0}},
                            {{"bone", "d"}, {"mins", {0, 0, 0}}, {"maxs", {1e33, 1, 1}}, {"offset", {0, 0, 0}}, {"radiusSquared", nullptr}}}}};
    const auto untouched = model["boneInfo"][2];
    CHECK(w2b::models::RepairHitboxes(model) == 3);
    CHECK(std::isfinite(static_cast<float>(model["boneInfo"][3]["radiusSquared"].get<double>())));
    CHECK(model["boneInfo"][0]["radiusSquared"].get<double>() == 9.0);
    CHECK(model["boneInfo"][1]["radiusSquared"].get<double>() == 0.0);
    CHECK(model["boneInfo"][1]["maxs"] == w2b::util::Json::array({0.0, 0.0, 0.0}));
    CHECK(model["boneInfo"][2] == untouched);
}

TEST_CASE("notetrack sounds pair words across lines", "[pipeline]")
{
    w2b::weapons::Schema schema{{"notetrackSoundMap", "WFT_NOTETRACKSOUNDMAP"}};
    w2b::weapons::InfoFields source;
    source.fields = {{"notetrackSoundMap", "\n\nsndnt#a mag_out\n\nsndnt#b\nmag_in\nhideTags\n"}};
    const auto weapon = w2b::weapons::Convert("w", source, schema, schema);
    CHECK(*weapon.fields.Find("notetrackSoundMap") == "sndnt#a waw/mag_out\nsndnt#b waw/mag_in");
}

TEST_CASE("weapon info strings read as WaW's Info_ValueForKey", "[pipeline]")
{
    const auto file = std::filesystem::temp_directory_path() / "w2b_info_test";
    w2b::files::WriteText(file, "WEAPONFILE\\clipSize\\8\\\\\\\\\\damage\\5\\clipSize\\9\\tail");
    const auto fields = w2b::weapons::ReadInfo(file);
    std::filesystem::remove(file);
    CHECK(*fields.Find("clipSize") == "8");
    CHECK(*fields.Find("damage") == "5");
    CHECK(fields.fields.size() == 2);
}

TEST_CASE("a guard on a dvar nothing sets folds", "[pipeline]")
{
    const std::set<std::string> assigned, unset{"nightmare_mode"};
    const auto fold = [&](const std::vector<std::string>& words) { return w2b::gsc::port::zombiebehavior::FoldLevelGuard(words, "m", &assigned, &unset); };
    CHECK(fold({"getdvarint", "(", "\"nightmare_mode\"", ")", "==", "0"}).value());
    CHECK_FALSE(fold({"getdvarint", "(", "\"nightmare_mode\"", ")", "==", "1"}).value());
    CHECK_FALSE(fold({"getdvar", "(", "\"nightmare_mode\"", ")", "==", "\"1\""}).value());
    CHECK_FALSE(fold({"getdvarint", "(", "\"nightmare_mode\"", ")"}).value());
    CHECK_FALSE(fold({"getdvarint", "(", "\"other\"", ")", "==", "0"}).has_value());
}

TEST_CASE("a spin loop stops on an undefined element", "[pipeline]")
{
    const auto lower = [](const std::string& text) {
        auto tokens = w2b::gsc::Tokenize(text);
        w2b::gsc::port::LowerUndefinedSpinLoops(tokens);
        return w2b::gsc::Emit(tokens);
    };
    CHECK(lower("while( players[ 0 ] != player_0 )\n\tplayers = array_randomize( players );") ==
          "while( isdefined( players[0] ) && ( players[ 0 ] != player_0 ) )\n\tplayers = array_randomize( players );");
    for (const auto* same : {"while( a[0] != b ) { wait 1; }", "while( a != b ) c = 1;", "while( isdefined( a[0] ) && a[0] != b ) c = 1;", "while( f( a[0] ) != b ) c = 1;"})
        CHECK(lower(same) == same);
}

TEST_CASE("static model palettes share when the index range is full", "[pipeline]")
{
    using w2b::world::gridPaletteSize;
    const auto palettes = [](std::initializer_list<int> values) {
        std::vector<std::uint8_t> bytes;
        for (const auto v : values)
            bytes.insert(bytes.end(), gridPaletteSize, static_cast<std::uint8_t>(v));
        return bytes;
    };
    const auto grid = palettes({10, 100});
    auto palette = palettes({12, 200, 12});
    w2b::util::Json placements{{"models", {{{"colorsIndex", 2}}, {{"colorsIndex", 3}}, {{"colorsIndex", 4}}, {{"colorsIndex", 1}}}}};
    auto room = palette;
    auto same = placements;
    CHECK(w2b::world::SharePalettes(grid, room, same, 5) == 0);
    CHECK(same == placements);
    // one free index: the two equal palettes take the closest grid entry, the far one keeps its own
    CHECK(w2b::world::SharePalettes(grid, palette, placements, 3) == 2);
    CHECK(palette == palettes({200}));
    CHECK(placements["models"] == w2b::util::Json::array({{{"colorsIndex", 0}}, {{"colorsIndex", 2}}, {{"colorsIndex", 0}}, {{"colorsIndex", 1}}}));
}

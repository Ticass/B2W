// build-mod helpers against expected csv parses and mod zones.
#include "audio/convert.h"
#include "core/files.h"
#include "pipeline/hud_menus.h"
#include "pipeline/impact_table.h"
#include "pipeline/ingame_menus.h"
#include "pipeline/script_menus.h"
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

TEST_CASE("skinned surfaces go first so the mod tools linker's vertex bases fit", "[pipeline]")
{
    // mesh 0: one triangle on bone 0 (rigid); mesh 1: a triangle across bones 0 and 1 (skinned)
    std::string bytes;
    const auto put = [&](const auto& value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof value); };
    for (const std::uint16_t index : {0, 1, 2, 3, 4, 5})
        put(index);
    for (const std::uint8_t bone : {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0})
        put(bone);
    for (int v = 0; v < 6; ++v)
        for (const float weight : {1.0f, 0.0f, 0.0f, 0.0f})
            put(weight);
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const auto chunk = static_cast<unsigned char>(bytes[i]) << 16 | (i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0);
        encoded += alphabet[chunk >> 18 & 63];
        encoded += alphabet[chunk >> 12 & 63];
        encoded += i + 1 < bytes.size() ? alphabet[chunk >> 6 & 63] : '=';
        encoded += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    const auto model = [&] {
        using Json = w2b::util::Json;
        const auto primitive = [](int indices) {
            return Json{{"indices", indices}, {"attributes", {{"POSITION", 4}, {"JOINTS_0", 2}, {"WEIGHTS_0", 3}}}};
        };
        return Json{{"buffers", {{{"uri", "data:application/octet-stream;base64," + encoded}}}},
                    {"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 12}}, {{"buffer", 0}, {"byteOffset", 12}, {"byteLength", 24}},
                                     {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 96}}}},
                    {"accessors", {{{"bufferView", 0}, {"componentType", 5123}, {"type", "SCALAR"}, {"count", 3}},
                                   {{"bufferView", 0}, {"byteOffset", 6}, {"componentType", 5123}, {"type", "SCALAR"}, {"count", 3}},
                                   {{"bufferView", 1}, {"componentType", 5121}, {"type", "VEC4"}, {"count", 6}},
                                   {{"bufferView", 2}, {"componentType", 5126}, {"type", "VEC4"}, {"count", 6}},
                                   {{"bufferView", 2}, {"componentType", 5126}, {"type", "VEC3"}, {"count", 6}}}},
                    {"meshes", {{{"primitives", {primitive(0)}}}, {{"primitives", {primitive(1)}}}}},
                    {"nodes", {{{"name", "skel"}, {"children", {1, 2}}}, {{"name", "surf0"}, {"mesh", 0}}, {{"name", "surf1"}, {"mesh", 1}}}}};
    };
    auto fits = model();
    CHECK(w2b::weapons::OrderSurfacesForLinker(fits, 3, 6));
    CHECK(fits["nodes"][0]["children"] == w2b::util::Json{2, 1});
    auto tooMany = model();
    CHECK_FALSE(w2b::weapons::OrderSurfacesForLinker(tooMany, 2, 6));
    CHECK(tooMany == model());
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

TEST_CASE("Compiled HUD expressions become RPN")
{
    using w2b::pipeline::HudExpression;
    const auto parse = [](const char* text) { return HudExpression(w2b::util::Json::parse(text)); };
    // the compiler leaves its outer parenthesis open: ( ( -70 - 50 )
    CHECK(parse(R"([{"op":16},{"op":16},{"op":6},{"int":70},{"op":6},{"int":50},{"op":1}])") == "n:70 op:neg n:50 op:-");
    CHECK(parse(R"([{"op":16},{"op":28},{"string":"a b"},{"op":1},{"op":11},{"int":1}])") == "s:a%20b f:dvarint:1 n:1 op:>=");
    // "[" + keybinding( "+actionslot 1" ) + "]"
    CHECK(parse(R"([{"op":16},{"string":"["},{"op":5},{"op":75},{"string":"+actionslot 1"},{"op":1},{"op":5},{"string":"]"}])") ==
          "s:%5b s:%2bactionslot%201 f:keybinding:1 op:+ s:%5d op:+");
    // !ui_active() && a == 0 || b
    CHECK(parse(R"([{"op":16},{"op":7},{"op":33},{"op":1},{"op":14},{"int":2},{"op":12},{"int":0},{"op":15},{"int":3}])") ==
          "f:ui_active:0 op:! n:2 n:0 op:== op:&& n:3 op:||");
    CHECK_THROWS(parse(R"([{"op":16},{"op":12},{"int":0}])"));
    CHECK(w2b::pipeline::HudFontRole(6, 0.3) == "objective");
    CHECK(w2b::pipeline::HudFontRole(4, 0.548) == "bold");
    CHECK(w2b::pipeline::HudFontRole(0, 0.381) == "extrabig");
}

TEST_CASE("Sounds decode as XAudio2 2.0 plays them")
{
    using namespace w2b;
    const auto bytes = [](const std::initializer_list<int> values) { std::string out; for (const int value : values) out += static_cast<char>(value); return out; };
    const auto put = [](std::string& out, const auto value) { out.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
    const auto chunk = [&](const char* tag, const std::string& body)
    {
        std::string out = tag;
        put(out, static_cast<uint32_t>(body.size()));
        out += body;
        if (body.size() & 1)
            out += bytes({0x00});
        return out;
    };
    const auto wave = [&](const uint16_t tag, const uint16_t channels, const uint16_t bits, const std::string& rest)
    {
        std::string fmt;
        put(fmt, tag);
        put(fmt, channels);
        put(fmt, uint32_t{48000});
        put(fmt, static_cast<uint32_t>(48000 * channels * bits / 8));
        put(fmt, static_cast<uint16_t>(channels * bits / 8));
        put(fmt, bits);
        const auto body = "WAVE" + chunk("fmt ", fmt) + rest;
        std::string out = "RIFF";
        put(out, static_cast<uint32_t>(body.size()));
        return out + body;
    };
    const auto folder = std::filesystem::temp_directory_path() / "w2b_sound_test";
    const auto decode = [&](const std::string& bytes)
    {
        const auto path = folder / "sound.wav";
        files::WriteBytes(path, bytes);
        const auto wav = audio::Convert(path, {}).first;
        REQUIRE(wav);
        const auto pcm = *audio::ParseChunks(*wav).Find("data");
        std::vector<int16_t> out(pcm.size() / 2);
        std::memcpy(out.data(), pcm.data(), out.size() * 2);
        return out;
    };
    using Samples = std::vector<int16_t>;
    CHECK(decode(wave(1, 1, 8, chunk("data", bytes({0x80, 0x00, 0xFF, 0x81})))) == Samples{0, -32768, 32512, 256});
    CHECK(decode(wave(1, 1, 24, chunk("data", bytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x80})))) == Samples{1, -32768});
    CHECK(decode(wave(1, 1, 32, chunk("data", bytes({0x00, 0x00, 0x02, 0x00})))) == Samples{2});
    std::string floats;
    for (const float value : {0.5f, -2.0f, 1.0f})
        put(floats, value);
    CHECK(decode(wave(3, 1, 32, chunk("data", floats))) == Samples{16384, -32768, 32767});
    // a partial frame is dropped; nothing after data counts, nor what comes before in it twice
    CHECK(decode(wave(1, 2, 16, chunk("PRIV", "a") + chunk("PRIV", "b") + chunk("smpl", "c") + chunk("data", bytes({0x01, 0x00, 0x02, 0x00, 0x03, 0x00})) + "cue"))
          == Samples{1, 2});
    CHECK(decode(wave(1, 1, 16, chunk("data", bytes({0x07, 0x00}))) + bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF})) == Samples{7});
    auto cut = wave(1, 1, 16, chunk("data", bytes({0x07, 0x00, 0x08, 0x00})));
    cut.resize(cut.size() - 2);
    files::WriteBytes(folder / "sound.wav", cut);
    CHECK_THROWS_AS(audio::Convert(folder / "sound.wav", {}), audio::SoundError);
    std::filesystem::remove_all(folder);
}

TEST_CASE("WaW's impact table becomes BO2's rows and surfaces")
{
    using namespace w2b;
    std::vector<std::vector<std::string>> rows(16, std::vector<std::string>(35));
    rows[0][13] = "impacts/metal"; // bullet_small_normal on metal
    rows[0][31] = "impacts/blood"; // body nonfatal
    rows[9][5] = "impacts/lost";   // grenade_bounce on concrete
    rows[14][0] = "maps/tesla";    // tank_shell, default surface
    const auto t6 = pipeline::T6Impacts(rows, {{"impacts/metal", "waw/m"}, {"impacts/blood", "waw/b"}, {"maps/tesla", "waw/t"}});
    REQUIRE(t6.size() == 21);
    for (const auto& row : t6)
        CHECK(row.size() == 36);
    CHECK(t6[0][13] == "waw/m");
    CHECK(t6[0][32] == "waw/b"); // after riotshield
    CHECK(t6[0][31].empty());
    CHECK(t6[17][0] == "waw/t");
    CHECK(t6[11][5].empty()); // grenade_bounce: not converted
    CHECK(t6[20][13] == "waw/m"); // blade, a hit WaW lacks
    CHECK_THROWS(pipeline::T6Impacts({}, {}));
}

TEST_CASE("A map's script menu runs its compiled scripts and keys")
{
    using namespace w2b;
    using util::Json;
    const Json window{{"name", "title"},
                      {"rect", {{"x", 85.0}, {"y", 320.0}, {"w", 100.0}, {"h", 32.0}, {"horzAlign", 0}, {"vertAlign", 0}}},
                      {"style", 1},
                      {"border", 0},
                      {"borderSize", 0.0},
                      {"ownerDraw", 0},
                      {"staticFlags", 0x100000},
                      {"dynamicFlags", 0x10004},
                      {"foreColor", {1.0, 1.0, 1.0, 1.0}},
                      {"backColor", {0.0, 0.0, 0.0, 0.5}},
                      {"borderColor", {0.0, 0.0, 0.0, 0.0}},
                      {"background", ",white"}};
    Json item{{"window", window},   {"type", 0},           {"font", 1},           {"textAlignMode", 1}, {"textAlignX", 0.0},   {"textAlignY", 0.0},
              {"textScale", 0.24},  {"textStyle", 0},      {"text", "Select Song"}, {"action", nullptr}, {"onFocus", nullptr},  {"leaveFocus", nullptr},
              {"mouseEnter", nullptr}, {"mouseExit", nullptr}, {"onKey", Json::array()}, {"visibleExp", nullptr}, {"textExp", nullptr},
              {"materialExp", nullptr}, {"rectXExp", nullptr}, {"rectYExp", nullptr}, {"rectWExp", nullptr}, {"rectHExp", nullptr}, {"forecolorAExp", nullptr}};
    auto hidden = item;
    hidden["window"]["dynamicFlags"] = 0;
    hidden["window"]["staticFlags"] = 0;
    const Json menu{{"window", window},
                    {"fullScreen", 0},
                    {"visibleExp", nullptr},
                    {"onOpen", "\"setDvar\" \"cl_bypassMouseInput\" 1 "},
                    {"onClose", nullptr},
                    {"onESC", "\"scriptMenuResponse\" 9 ; \"close\" \"tom\" "},
                    {"onKey", {{{"key", 49}, {"action", "\"scriptMenuResponse\" 1 ; \"close\" \"tom\" "}}}},
                    {"blurRadius", 0.0},
                    {"focusColor", {1.0, 1.0, 1.0, 1.0}},
                    {"items", {item, hidden}}};
    const auto project = std::filesystem::temp_directory_path() / ("w2b-script-menu-" + std::to_string(std::random_device{}()));
    const auto staged = pipeline::StageWawMenus(project, {}, "test", {{"tom", pipeline::CompiledMenuSource("tom", menu)}});
    // no Mod Tools sources: the pause menu stays BO2's, the map's menu converts
    REQUIRE(staged.error);
    REQUIRE(!staged.result.is_null());
    CHECK(staged.result["script_menus"] == Json::array({"tom"}));
    const auto table = files::ReadText(project / "zm/waw_menus.csv");
    CHECK(table.find("scriptmenus,tom") != std::string::npos);
    CHECK(table.find("scriptMenuResponse 9;close tom") != std::string::npos);
    CHECK(table.find("49=scriptMenuResponse 1;close tom") != std::string::npos);
    CHECK(table.find("setDvar cl_bypassMouseInput 1") != std::string::npos);
    CHECK(table.find("item:tom:1,85 320 100 32 0 0,1,white,1 1 1 1,0 0 0 0.5") != std::string::npos);
    CHECK(table.find("Select Song") != std::string::npos);
    CHECK(table.find("n:1") != std::string::npos);
    CHECK(table.find("n:0") != std::string::npos); // the item without "visible 1"
    std::filesystem::remove_all(project);

    // a raw/ui another mod edited that does not parse: the pause menu stays BO2's, the map's menus still convert
    const auto raw = project.string() + "-raw";
    std::filesystem::create_directories(std::filesystem::path(raw) / "ui");
    files::WriteText(std::filesystem::path(raw) / "ui/ingame.txt", "{\n    loadMenu { \"ui/broken.menu\" }\n}\n");
    files::WriteText(std::filesystem::path(raw) / "ui/hud.txt", "{\n}\n");
    files::WriteText(std::filesystem::path(raw) / "ui/broken.menu", "#define F(a) a\nF(1\n");
    const auto broken = pipeline::StageWawMenus(project, {raw}, "test", {{"tom", pipeline::CompiledMenuSource("tom", menu)}});
    REQUIRE(broken.error);
    CHECK(broken.error->find("not found") == std::string::npos); // the sources were found and failed to parse
    REQUIRE(!broken.result.is_null());
    CHECK(broken.result["script_menus"] == Json::array({"tom"}));
    std::filesystem::remove_all(project);
    std::filesystem::remove_all(raw);
}

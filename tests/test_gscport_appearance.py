import tempfile
import unittest
import zipfile
from pathlib import Path

from waw2bo2 import gsc, gscport


class AppearanceTests(unittest.TestCase):
    def test_iwd_script_wins_over_zone_rawfile(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "zone/maps").mkdir(parents=True)
            (base / "zone/maps/_loadout.gsc").write_text("init_loadout() {}\n")
            (base / "zone/maps/only_zone.gsc").write_text("f() {}\n")
            with zipfile.ZipFile(base / "mod.iwd", "w") as z:
                z.writestr("maps/_loadout.gsc", "init_loadout() {}\ngive_model( class ) {}\n")
            sources = gscport.Sources([base / "zone"], [base / "mod.iwd"], None)
            self.assertEqual(sources.origin["maps\\_loadout.gsc"], "mod.iwd:maps/_loadout.gsc")
            self.assertTrue(gscport.has_waw_appearance(sources))
            self.assertIn("maps\\only_zone.gsc", sources.text)

    def test_level_script_reads_the_waw_map_name(self):
        tokens = gsc.tokenize('f() { if ( level.script == "x" ) self.script = 1; level.script_x = 2; }')
        self.assertEqual(gscport.rename_level_fields(tokens), 1)
        text = gsc.emit(tokens)
        self.assertIn("level.waw_script ==", text)
        self.assertIn("self.script = 1", text)
        self.assertIn("level.script_x", text)
        self.assertIn('level.waw_script = "nuketown";', gscport.assets_source({}, "nuketown", {"gun": "gun"}))
        self.assertIn('level.waw2bo2_weapons["gun"] = "gun";', gscport.assets_source({}, None, {"gun": "gun"}))

    def test_bo2_character_hooks_point_at_the_waw_appearance(self):
        with tempfile.TemporaryDirectory() as temp:
            main = Path(temp) / "map.gsc"
            main.write_text("zclassic_init()\n{\n    level.precachecustomcharacters = ::precache_x;\n"
                            "    level.givecustomcharacters = ::give_x;\n}\n")
            self.assertTrue(gscport.hook_bo2_characters(main))
            self.assertFalse(gscport.hook_bo2_characters(main))
            text = main.read_text()
            self.assertIn("level.givecustomcharacters = maps\\mp\\waw\\_waw2bo2_characters::give;", text)
            self.assertIn("level.precachecustomcharacters = maps\\mp\\waw\\_waw2bo2_characters::precache;", text)
            source = gscport.characters_source()
            self.assertIn("maps\\mp\\waw\\_loadout::give_model( self.pers[\"class\"] );", source)
            self.assertIn("maps\\mp\\waw\\_loadout::init_loadout();", source)
            main.write_text("main() {}\n")
            with self.assertRaises(ValueError):
                gscport.hook_bo2_characters(main)


class WeaponRegistrationTests(unittest.TestCase):
    def test_bo2_template_weapon_lists_are_replaced_by_the_waw_registration(self):
        with tempfile.TemporaryDirectory() as temp:
            main = Path(temp) / "map.gsc"
            main.write_text("main()\n{\n    level._zombie_custom_add_weapons = ::custom_add_weapons;\n"
                            "    include_weapons();\n}\n\ninclude_weapons()\n{\n    include_weapon( \"m14_zm\" );\n}\n")
            self.assertTrue(gscport.hook_bo2_weapons(main))
            self.assertFalse(gscport.hook_bo2_weapons(main))
            text = main.read_text()
            self.assertIn("    maps\\mp\\waw\\_waw2bo2_weapons::include_weapons();", text)
            self.assertIn("level._zombie_custom_add_weapons = maps\\mp\\waw\\_waw2bo2_weapons::add_weapons;", text)
            self.assertIn("\ninclude_weapons()\n{", text)   # the template definition itself is untouched
        source = gscport.weapons_source({"include": ["dlc3_code__include_weapons"], "add": [None]})
        self.assertIn("maps\\mp\\waw\\_waw2bo2_core::dlc3_code__include_weapons();", source)
        self.assertIn('include_zombie_weapon( "knife_zm", 0 );', source)
        self.assertNotIn("None", source)

    def test_wall_buys_follow_the_carried_weapon_table(self):
        import json
        from waw2bo2 import entities
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "entities.json"
            path.write_text(json.dumps({"entities": [
                {"targetname": "weapon_upgrade", "zombie_weapon_upgrade": "fs_m14"},
                {"targetname": "weapon_upgrade", "zombie_weapon_upgrade": "zombie_knuckle_crack"},
                {"targetname": "weapon_upgrade", "zombie_weapon_upgrade": "rgun"},
                {"targetname": "weapon_upgrade", "zombie_weapon_upgrade": "m14_zm"}]}))
            notes = entities.map_wall_buys(path, {"fs_m14": "fs_m14", "zombie_knuckle_crack": "waw_zombie_knuckle_crack",
                                                  "rgun": ""})
            weapons = [e["zombie_weapon_upgrade"] for e in json.loads(path.read_text())["entities"]]
            self.assertEqual(weapons, ["fs_m14", "waw_zombie_knuckle_crack", "m14_zm"])
            self.assertTrue(any(n.startswith("UNSUPPORTED_WALLBUY rgun") for n in notes))


if __name__ == "__main__":
    unittest.main()

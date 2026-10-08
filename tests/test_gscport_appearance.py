import tempfile
import unittest
import zipfile
from pathlib import Path

from waw2bo2 import gsc, gscport


class StationaryFxTests(unittest.TestCase):
    emitter = '''emitter(loop) {
        fx = self.script_noteworthy;
        fxTag = "tag_origin";
        self.fx = spawn("script_model", self.origin);
        self.fx setmodel("tag_origin");
        self.fx.angles = self.angles;
        self.fx.origin = self.origin;
        self.fx linkto(self, fxTag);
        if (isdefined(loop)) playloopedfx(level._effect[fx], self.speed, self.origin);
        else playfxontag(level._effect[fx], self.fx, fxTag);
    }'''

    def test_stationary_fx_keeps_asset_transform_and_loop_timing_without_model(self):
        tokens = gsc.tokenize(self.emitter)
        self.assertEqual(gscport.lower_stationary_fx_carriers(tokens), 1)
        gscport.rename_level_fields(tokens)
        result = gsc.emit(tokens)
        self.assertNotIn('spawn("script_model"', result)
        self.assertIn('::waw_playfx(level.waw_effect[fx], self.origin,', result)
        self.assertIn('anglestoforward(self.angles), anglestoup(self.angles)', result)
        self.assertIn('playloopedfx(level.waw_effect[fx], self.speed, self.origin)', result)
        self.assertEqual(gscport.lower_stationary_fx_carriers(tokens), 0)

    def test_moving_triggered_or_externally_used_carriers_are_preserved(self):
        for extra in ['self waittill("trigger");', 'self.fx moveto((1,2,3), 1);',
                      'other = self.fx;', 'self.fx delete();']:
            with self.subTest(extra=extra):
                tokens = gsc.tokenize(self.emitter.replace('fx = self.script_noteworthy;',
                                                          extra + 'fx = self.script_noteworthy;'))
                self.assertEqual(gscport.lower_stationary_fx_carriers(tokens), 0)
                self.assertEqual(gsc.emit(tokens), self.emitter.replace('fx = self.script_noteworthy;',
                                                                      extra + 'fx = self.script_noteworthy;'))


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
    def test_offhand_save_and_restore_use_weapon_translation(self):
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "maps").mkdir()
            (root / "maps/test.gsc").write_text(
                'main(){ x = self getcurrentoffhand(); self switchtooffhand(x); }')
            translator = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}),
                                           gscport.PortReport())
            source = translator.translate(r"maps\test")
            self.assertIn("::waw_getcurrentoffhand()", source)
            self.assertIn("::waw_switchtooffhand(x)", source)

    def test_shared_primary_frag_has_one_stable_reverse_name(self):
        source = gscport.assets_source({}, weapons={
            "stielhandgranate": "frag_grenade_zm", "fraggrenade": "frag_grenade_zm",
            "zombie_cymbal_monkey": "zombie_cymbal_monkey"})
        self.assertIn('level.waw2bo2_weapons["stielhandgranate"] = "frag_grenade_zm";', source)
        self.assertIn('level.waw2bo2_weapons["fraggrenade"] = "frag_grenade_zm";', source)
        self.assertEqual(source.count('level.waw2bo2_weapon_names["frag_grenade_zm"]'), 1)
        self.assertIn('level.waw2bo2_weapon_names["frag_grenade_zm"] = "fraggrenade";', source)
        self.assertIn('level.waw2bo2_runtime_weapons["zombie_cymbal_monkey"] = "cymbal_monkey_zm";', source)

    def test_monkey_runtime_support_preserves_source_registration(self):
        source = gscport.assets_source({}, weapons={"zombie_cymbal_monkey": "zombie_cymbal_monkey"})
        self.assertIn('level.waw2bo2_weapons["zombie_cymbal_monkey"] = "zombie_cymbal_monkey";', source)
        self.assertIn('level.waw2bo2_runtime_weapons["zombie_cymbal_monkey"] = "cymbal_monkey_zm";', source)
        self.assertIn('level.waw2bo2_weapon_names["cymbal_monkey_zm"] = "zombie_cymbal_monkey";', source)
        # Absent/excluded source weapons must not activate tactical gameplay.
        for weapons in ({}, {"zombie_cymbal_monkey": ""}, {"zombie_cymbal_monkey": "cymbal_monkey_zm"}):
            self.assertNotIn("waw2bo2_runtime_weapons", gscport.assets_source({}, weapons=weapons))

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


class WawBoxExtractionTests(unittest.TestCase):
    WEAPONS = (
        '#using_animtree( "nuketown" );\n'
        "init() { init_weapons(); level.boxAnim = []; level.boxAnim[\"open\"] = %box_open;\n"
        "         level.boxAnim[\"fake\"] = %box_fake; level.unused = 1; }\n"
        "init_weapons() {}\n"
        "treasure_chest_init() { level.chests = getentarray( \"c\", \"targetname\" );\n"
        "                        array_thread( level.chests, ::treasure_chest_think ); }\n"
        "treasure_chest_think() { self useanimtree( #animtree ); self setanim( level.boxAnim[\"open\"] ); }\n")

    def test_box_extracts_its_siblings_animations_and_level_state(self):
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "zone/maps").mkdir(parents=True)
            (base / "zone/maps/_zombiemode_weapons.gsc").write_text(self.WEAPONS)
            sources = gscport.Sources([base / "zone"], [], None)
            # BO2's own box defines a function of the same name
            api = T6Api({"getentarray": (2, 2)}, {"maps\\mp\\zombies\\_zm_magicbox": {"treasure_chest_think": 0}},
                        methods={"useanimtree": (1, 1), "setanim": (1, 4)})
            tr = gscport.Translator(sources, api, gscport.PortReport())
            tr.core_animtrees = {"nuketown": {"box_open"}}
            entry = tr.extract_entry("maps\\_zombiemode_weapons", "treasure_chest_init")
            parts = []
            while tr.core_queue:
                parts.append(tr.translate_extracted(*tr.core_queue.pop(0)))
            state = tr.extract_level_state("maps\\_zombiemode_weapons", "init")
        code = "\n".join(parts)
        self.assertEqual(entry, "zombiemode_weapons__treasure_chest_init")
        self.assertIn("::zombiemode_weapons__treasure_chest_think", code)
        self.assertNotIn("_zm_magicbox", code)
        self.assertIn("level.waw_chests", code)
        self.assertIn('#using_animtree( "nuketown" );\nzombiemode_weapons__treasure_chest_think', code)
        self.assertEqual(state, "zombiemode_weapons__init_level_state")
        state_code = tr.level_state_parts[0]
        self.assertIn("level.boxAnim[\"open\"] = %box_open;", state_code)
        self.assertIn("level.boxAnim[\"fake\"] = undefined;", state_code)    # no WaW xanim
        self.assertNotIn("level.unused", state_code)    # not read by the box
        self.assertNotIn("init_weapons", state_code)    # BO2 owns the rest of init


class ExtractedAnimtreeTests(unittest.TestCase):
    def test_animtree_without_anims_from_unstaged_tree_is_neutralized(self):
        from waw2bo2.t6api import T6Api
        source = ('#using_animtree( "generic_human" );\n'
                  'dronespawn( spawner ) { drone = spawn( "script_model", spawner.origin ); '
                  'drone useanimtree( #animtree ); }\n')
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "zone/maps").mkdir(parents=True)
            (base / "zone/maps/_spawner.gsc").write_text(source)
            sources = gscport.Sources([base / "zone"], [], None)
            api = T6Api({"spawn": (2, 6)}, {}, methods={"useanimtree": (1, 1)})
            tr = gscport.Translator(sources, api, gscport.PortReport())
            tr.extract_entry("maps\\_spawner", "dronespawn")
            code = tr.translate_extracted(*tr.core_queue.pop(0))
        self.assertNotIn("#animtree", code)


class BuiltinShadowTests(unittest.TestCase):
    def test_map_function_named_like_a_builtin_is_renamed_with_its_calls(self):
        from waw2bo2.t6api import T6Api
        util = ("getFirstArrayKey( array ) { keys = getarraykeys( array ); return keys[0]; }\n"
                "first( a ) { k = getFirstArrayKey( a ); f = ::getFirstArrayKey; return k; }\n")
        user = "use( a ) { return maps\\_bo2_util::getFirstArrayKey( a ); }\n"
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "zone/maps").mkdir(parents=True)
            (base / "zone/maps/_bo2_util.gsc").write_text(util)
            (base / "zone/maps/_user.gsc").write_text(user)
            sources = gscport.Sources([base / "zone"], [], None)
            api = T6Api({"getfirstarraykey": (1, 1), "getarraykeys": (1, 1)}, {})
            tr = gscport.Translator(sources, api, gscport.PortReport())
            util_out = tr.translate("maps\\_bo2_util")
            user_out = tr.translate("maps\\_user")
        self.assertIn("waw_getFirstArrayKey( array )", util_out)
        self.assertIn("k = waw_getFirstArrayKey( a )", util_out)
        self.assertIn("::waw_getFirstArrayKey", util_out)
        self.assertIn("getarraykeys( array )", util_out)            # a real builtin call stays
        self.assertIn("::waw_getFirstArrayKey( a )", user_out)


class LibraryFunctionTests(unittest.TestCase):
    def test_trem_hintstrings_calls_become_native_hint_strings(self):
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            (base / "zone/maps").mkdir(parents=True)
            (base / "zone/maps/trem_hintstrings.gsc").write_text("_setHintString( string ) {}\n")
            (base / "zone/maps/door.gsc").write_text(
                "#include maps\\trem_hintstrings;\n"
                "a() { self maps\\trem_hintstrings::_setHintString( \"Press &&1\" ); }\n"
                "b() { self _setHintString( \"Press &&1\" ); }\n")
            sources = gscport.Sources([base / "zone"], [], None)
            report = gscport.PortReport(map_main="maps\\door")
            tr = gscport.Translator(sources, T6Api({"sethintstring": (1, 2)}, {}), report)
            text = tr.translate("maps\\door")
        compat = r"maps\mp\waw\_waw2bo2_compat::waw_native_hintstring"
        self.assertEqual(text.count(compat), 2, text)
        self.assertNotIn("trem_hintstrings::_sethintstring", text.lower())

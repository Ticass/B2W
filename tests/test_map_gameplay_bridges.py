import tempfile
import json
import unittest
from pathlib import Path

from waw2bo2 import gscport, t6bridge, weapons
from waw2bo2.t6api import T6Api

STOCK_DAMAGE = ("player_damage_override( eInflictor, eAttacker, iDamage, iDFlags, sMeansOfDeath, sWeapon, vPoint, "
                "vDir, sHitLoc, modelIndex, psOffsetTime )\n{\n\tif( sMeansOfDeath == \"MOD_FALLING\" )\n\t{\n"
                "\t\tsMeansOfDeath = \"MOD_EXPLOSIVE\";\n\t}\n"
                "\tself maps\\_callbackglobal::finishPlayerDamageWrapper( eInflictor, eAttacker, iDamage, iDFlags, "
                "sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, modelIndex, psOffsetTime );\n}\n")


def _api() -> T6Api:
    return T6Api(builtins={"getentarray": (2, 2)},
                 scripts={"maps\\mp\\zombies\\_zm": {"init": 0, "register_player_damage_callback": 1}},
                 methods={"hasweapon": (1, 1)})


class DamagePreludeTests(unittest.TestCase):
    def test_catalogued_new_stock_damage_body_is_not_a_custom_prelude(self):
        head = STOCK_DAMAGE.split('{\n', 1)[0]
        old = head + '{\nif (iDamage < self.health) { return; }\n}\n'
        newer = head + '''{
            if (sMeansOfDeath == "MOD_FALLING") sMeansOfDeath = "MOD_EXPLOSIVE";
            if (self.health > 75) {
                self maps\\_callbackglobal::finishPlayerDamageWrapper(eInflictor, eAttacker, 75,
                    iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, modelIndex, psOffsetTime);
                return;
            }
            if (iDamage < self.health) { return; }
        }\n'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for home, text in ((root / 'map', newer), (root / 'stock', old), (root / 'factory', newer)):
                (home / 'maps').mkdir(parents=True)
                (home / 'maps/_zombiemode.gsc').write_text('// map-local copy\n' + text if home.name == 'map' else text)
            before = gscport.PortReport()
            gscport.Translator(gscport.Sources([root / 'map'], [], root / 'stock'), _api(), before).extract_damage_prelude()
            self.assertTrue(any('prelude applies damage itself' in e for e in before.unsupported.get(gscport.FALLBACK, [])))
            (root / 'stock/catalog.json').write_text(json.dumps({'zones': {
                'zone/english/factory.ff': {'folder': str(root / 'factory')}}}))
            sources = gscport.Sources([root / 'map'], [], root / 'stock')
            self.assertIn(newer, sources.stock_variants('maps\\_zombiemode'))
            report = gscport.PortReport()
            translator = gscport.Translator(sources, _api(), report)
            self.assertIsNone(translator.extract_damage_prelude())
            self.assertEqual(report.errors, [])
            # An authored rule before the newer stock body is still retained.
            (root / 'map/maps/_zombiemode.gsc').write_text(newer.replace('{\n', '{\nif (self.shielded) return;\n', 1))
            edited = gscport.PortReport()
            translator = gscport.Translator(gscport.Sources([root / 'map'], [], root / 'stock'), _api(), edited)
            self.assertIsNotNone(translator.extract_damage_prelude())
            self.assertEqual(edited.errors, [])
            self.assertIn('self.shielded', '\n'.join(translator.level_state_parts))

    def test_dlc_powerup_callback_preserves_source_parameter_and_registration(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode_powerups.gsc').write_text('include_powerup(name) {}')
            (root / 'maps/dlc3_code.gsc').write_text('''
                #include maps\\_zombiemode_powerups;
                include_powerups(use_dlc4_powerups) {
                    include_powerup("nuke");
                    if(isdefined(use_dlc4_powerups) && use_dlc4_powerups)
                        include_powerup("fire_sale");
                }''')
            report = gscport.PortReport()
            api = _api()
            api.builtins['isdefined'] = (1, 1)
            translator = gscport.Translator(gscport.Sources([root], [], None), api, report)
            reference = translator.resolve_core(r'maps\dlc3_code', 'include_powerups', None, 'map', set())
            self.assertIn('_waw2bo2_core', reference)
            body = translator.translate_extracted(r'maps\dlc3_code', 'include_powerups')
            self.assertIn('include_powerups(use_dlc4_powerups)', body)
            self.assertIn('include_zombie_powerup', body)
            self.assertIn('"fire_sale"', body)
            self.assertNotIn('include_powerups', ''.join(translator.stubs))

    def port(self, override: str, zone_stock: str = STOCK_DAMAGE) -> tuple[str, gscport.PortReport]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {root / "src/maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n}\n",
                     root / "src/maps/_zombiemode.gsc": "main() {}\n" + override,
                     root / "stock/maps/_zombiemode.gsc": "main() {}\n" + zone_stock,
                     root / "raw/maps/_zombiemode.gsc": "main() {}\n" + STOCK_DAMAGE}
            for path, text in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            sources = gscport.Sources([root / "src"], [], root / "stock", [root / "raw"])
            report = gscport.port_map(sources, _api(), "testmap", root / "out")
            core = (root / "out/maps/mp/waw/_waw2bo2_core.gsc").read_text(encoding="utf-8")
            return core, report

    def test_map_rules_before_the_stock_body_become_a_bo2_damage_callback(self):
        # e.g. a buildable shield absorbing zombie hits while worn on the back
        prelude = "\tif( self HasWeapon( \"zombie_shield\" ) )\n\t{\n\t\tiDamage = 0;\n\t\treturn;\n\t}\n"
        body = STOCK_DAMAGE.split("{\n", 1)
        core, report = self.port(body[0] + "{\n" + prelude + body[1])
        self.assertIn("zombiemode__player_damage_override_prelude( eInflictor, eAttacker, iDamage, iDFlags, "
                      "sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, psOffsetTime )", core)
        self.assertIn("return 0;", core)                  # WaW bare return = hit dropped
        self.assertIn("return -1;", core)                 # fall through: no opinion, later callbacks run
        self.assertIn("if ( iDamage != waw2bo2_damage_in )\n\t\treturn iDamage;", core)  # changed damage kept
        self.assertNotIn("MOD_FALLING", core)             # BO2 _zm keeps the stock damage logic
        self.assertIn("register_player_damage_callback( ::zombiemode__player_damage_override_prelude )", core)
        self.assertIn("\tzombiemode__player_damage_override_prelude_register();", core)
        self.assertFalse([e for e in report.unsupported.get(gscport.FALLBACK, []) if "damage" in e])

    def test_map_edited_from_the_raw_release_matches_it_not_the_zone_one(self):
        # stock zones carry an older _zombiemode than the raw scripts maps start from
        older = STOCK_DAMAGE.replace("\tif( sMeansOfDeath",
                                     "\tif( iDamage < self.health )\n\t{\n\t\treturn;\n\t}\n\tif( sMeansOfDeath")
        body = STOCK_DAMAGE.split("{\n", 1)
        core, report = self.port(body[0] + "{\n\tif( self.shielded )\n\t\treturn;\n" + body[1], older)
        self.assertIn("if( self.shielded )\n\t\treturn 0;", core)
        self.assertFalse([e for e in report.unsupported.get(gscport.FALLBACK, []) if "damage" in e])

    def port_callbackglobal(self, prelude: str, bo2_stock_perks: bool = False) -> tuple[str, gscport.PortReport]:
        stock = ("Callback_PlayerDamage( eInflictor, eAttacker, iDamage, iDFlags, sMeansOfDeath, sWeapon, vPoint, "
                 "vDir, sHitLoc, modelIndex, psOffsetTime )\n{\n"
                 "\tif( isdefined( eAttacker ) && isPlayer( eAttacker ) )\n\t{\n\t\treturn;\n\t}\n"
                 "\tself finishPlayerDamageWrapper( eInflictor, eAttacker, iDamage, iDFlags, sMeansOfDeath, "
                 "sWeapon, vPoint, vDir, sHitLoc, modelIndex, psOffsetTime );\n}\n")
        head, tail = stock.split("{\n", 1)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {root / "src/maps/testmap.gsc": "main()\n{\n\tmaps\\_zombiemode::main();\n}\n",
                     root / "src/maps/_callbackglobal.gsc": "init() {}\n" + head + "{\n" + prelude + tail,
                     root / "stock/maps/_callbackglobal.gsc": "init() {}\n" + stock,
                     root / "stock/maps/_zombiemode.gsc": "main() {}\n" + STOCK_DAMAGE}
            for path, text in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            sources = gscport.Sources([root / "src"], [], root / "stock")
            api = _api()
            api.methods["hasperk"] = (1, 1)
            report = gscport.port_map(sources, api, "testmap", root / "out", bo2_stock_perks=bo2_stock_perks)
            return (root / "out/maps/mp/waw/_waw2bo2_core.gsc").read_text(encoding="utf-8"), report

    PHD = ("\tif( self HasPerk(\"specialty_detectexplosive\") && sMeansOfDeath == \"MOD_FALLING\" )\n\t{\n"
           "\t\tself.previous_health = self.health;\n\t\treturn;\n\t}\n")

    def test_callbackglobal_player_damage_rules_become_a_bo2_damage_callback(self):
        # Nuketown's PhD Flopper: the map's _callbackglobal cancels explosive and fall damage
        core, report = self.port_callbackglobal(self.PHD)
        self.assertIn("callbackglobal__callback_playerdamage_prelude( eInflictor, eAttacker, iDamage, iDFlags, "
                      "sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, psOffsetTime )", core)
        self.assertIn("specialty_detectexplosive", core)
        self.assertIn("return 0;", core)                  # the immunity drops the hit
        self.assertNotIn("isPlayer( eAttacker )", core)   # stock body stays BO2's
        self.assertIn("register_player_damage_callback( ::callbackglobal__callback_playerdamage_prelude )", core)
        self.assertIn("\tcallbackglobal__callback_playerdamage_prelude_register();", core)
        self.assertFalse([e for e in report.unsupported.get(gscport.FALLBACK, []) if "damage" in e.lower()])

    def test_perk_damage_rules_are_dropped_when_bo2_owns_the_perks(self):
        # BO2's own PhD Flopper (_zm_perk_divetonuke) replaces the WaW one
        shield = "\tif( self.shielded )\n\t\treturn;\n"
        core, report = self.port_callbackglobal(self.PHD + shield, bo2_stock_perks=True)
        self.assertNotIn("previous_health", core)
        self.assertIn("if( self.shielded )", core)        # other map rules stay
        core, _ = self.port_callbackglobal(self.PHD, bo2_stock_perks=True)
        self.assertNotIn("callbackglobal__callback_playerdamage_prelude", core)

    def test_stock_damage_override_registers_nothing(self):
        core, _ = self.port(STOCK_DAMAGE)
        self.assertNotIn("prelude", core)

    def test_edited_stock_branch_still_ends_the_custom_prelude(self):
        body = STOCK_DAMAGE.split("{\n", 1)
        edited = body[1].replace('sMeansOfDeath = "MOD_EXPLOSIVE";',
                                 'if ( level.easter_egg != 4 ) sMeansOfDeath = "MOD_EXPLOSIVE"; else return;')
        core, report = self.port(body[0] + '{\n\tif ( self.self_revive ) return;\n' + edited)
        self.assertIn('if ( self.self_revive ) return 0;', core)
        self.assertNotIn('easter_egg', core)
        self.assertFalse([e for e in report.unsupported.get(gscport.FALLBACK, []) if 'damage' in e])

    def test_statement_split_keeps_if_else_chains_whole(self):
        tokens = gscport.gsc.tokenize("{ if ( a ) { b(); } else c(); d = 1; for ( i = 0; i < 2; i++ ) e(); }")
        self.assertEqual(len(gscport.top_level_statements(tokens, 0, len(tokens) - 2)), 3)

    def test_player_damage_multiplier_comes_from_source_gameskill(self):
        # Verruckt's trap DoDamage( 50 ) lands as 16 in WaW solo (100 / 310)
        import tempfile
        from pathlib import Path
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_gameskill.gsc').write_text(
                'setskill(){ add_fractional_data_point( "playerDifficultyHealth", 0.25, 475 ); '
                'add_fractional_data_point( "playerDifficultyHealth", 0.75, 310 ); '
                'add_fractional_data_point( "playerDifficultyHealth", 1.0, 210 ); '
                + ''.join(f'level.difficultySettings[ "coopPlayerDifficultyHealth" ][ "normal" ][{i}] = {v}; '
                          for i, v in enumerate((1.0, 0.9, 0.8, 0.7)))
                + 'difficulty_starting_frac[ "normal" ] = 0.75; }')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.player_damage_scale_hook(),
                             ['level thread maps\\mp\\waw\\_waw2bo2_core::waw2bo2_player_damage_scale();'])
            self.assertIn('scales[0] = 0.3225806451612903;', tr.level_state_parts[-1])
            self.assertIn('scales[3] = 0.4608294930875576;', tr.level_state_parts[-1])

    def test_player_regen_timing_comes_from_source_gameskill(self):
        # a map whose _gameskill regenerates sooner than BO2's 2400 ms constant
        import tempfile
        from pathlib import Path
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_gameskill.gsc').write_text(
                'setskill(){ add_fractional_data_point( "playerHealth_RegularRegenDelay", 0.25, 3000 ); '
                'add_fractional_data_point( "playerHealth_RegularRegenDelay", 1.0, 1500 ); '
                'add_fractional_data_point( "healthOverlayCutoff", 0.75, 0.2 ); '
                'difficulty_starting_frac[ "normal" ] = 0.75; }')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.player_health_regen_hook(),
                             ['maps\\mp\\waw\\_waw2bo2_core::waw2bo2_player_regen_timing();'])
            self.assertIn('level.playerhealth_regularregendelay = 2000;', tr.level_state_parts[-1])
            self.assertIn('level.healthoverlaycutoff = 0.2;', tr.level_state_parts[-1])
            self.assertNotIn('longregentime', tr.level_state_parts[-1])
            self.assertTrue(any('longRegenTime' in e for e in tr.report.unsupported[gscport.FALLBACK]))

    def test_spawner_lists_come_from_source_main(self):
        # Der Riese: _zombiemode::main( "receiver_zone_spawners" ) picks the
        # initial spawners its own round_spawning and zone manager use
        import tempfile
        from pathlib import Path
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode.gsc').write_text(
                'main(init_zombie_spawner_name){ precache_shaders(); '
                'if( !IsDefined( init_zombie_spawner_name) ) { level.enemy_spawns = getEntArray( "zombie_spawner_init", "targetname" ); } '
                'else { level.enemy_spawns = getEntArray( init_zombie_spawner_name, "targetname" ); } '
                'level.zombie_rise_spawners = []; level.custom_introscreen = ::intro; maps\\_load::main(); }')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.extract_spawner_lists(), 'zombiemode__main_spawner_lists')
            text = tr.level_state_parts[-1]
            self.assertIn('zombiemode__main_spawner_lists( init_zombie_spawner_name )', text)
            self.assertIn('( init_zombie_spawner_name, "targetname" )', text)
            self.assertIn('level.zombie_rise_spawners = [];', text)
            self.assertNotIn('custom_introscreen', text)
            self.assertNotIn('precache_shaders', text)

    def test_player_connect_fields_come_from_source_onplayerconnect(self):
        import tempfile
        from pathlib import Path
        from waw2bo2.t6api import T6Api
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'maps').mkdir()
            (root / 'maps/_zombiemode.gsc').write_text(
                'onPlayerConnect() { for( ;; ) { level waittill( "connecting", player ); '
                'player.entity_num = player GetEntityNumber(); player thread onPlayerSpawned(); '
                'player.score = level.zombie_vars["zombie_score_start"]; player.old_score = player.score; } }')
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertEqual(tr.extract_player_connect_fields(), 'zombiemode__onplayerconnect_fields')
            text = tr.level_state_parts[-1]
            self.assertIn('level waittill( "connecting", player );', text)
            self.assertIn('player.entity_num =', text)
            self.assertIn('player.pers["score"] = player.score;', text)
            self.assertIn('player.old_score = player.score;', text)
            self.assertNotIn('onPlayerSpawned', text)

    def test_statement_split_skips_developer_blocks(self):
        # Verruckt round_start: level.chalk_hud1 follows a /# #/ block
        tokens = gscport.gsc.tokenize("{ a = 1; /# b(); #/ level.x = c(); }")
        spans = gscport.top_level_statements(tokens, 0, len(tokens) - 2)
        self.assertEqual([tokens[s].text for s, _ in spans], ["a", "level"])

    def test_zero_damage_finish_then_return_becomes_callback_cancellation(self):
        body = STOCK_DAMAGE.split("{\n", 1)
        prelude = '''if (self.second_chance) {
            self maps\\_callbackglobal::finishPlayerDamageWrapper(eInflictor, eAttacker, 0,
                iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, modelIndex, psOffsetTime);
            return;
        }\n'''
        core, report = self.port(body[0] + '{\n' + prelude + body[1])
        self.assertIn('iDamage = 0;', core)
        self.assertIn('return 0;', core)
        self.assertNotIn('finishPlayerDamageWrapper', core)
        self.assertFalse([e for e in report.unsupported.get(gscport.FALLBACK, []) if 'damage' in e])

    def test_positive_damage_finish_is_still_rejected(self):
        body = STOCK_DAMAGE.split("{\n", 1)
        prelude = '''if (self.custom_damage) {
            self maps\\_callbackglobal::finishPlayerDamageWrapper(eInflictor, eAttacker, 75,
                iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, modelIndex, psOffsetTime);
            return;
        }\n'''
        _, report = self.port(body[0] + '{\n' + prelude + body[1])
        self.assertTrue([e for e in report.unsupported.get(gscport.FALLBACK, []) if 'prelude applies damage itself' in e])


class HeldItemMeleeTests(unittest.TestCase):
    T4 = {"inventoryType": "WFT_INVENTORYTYPE", "meleeAnim": "WFT_ANIM_NAME", "meleeDamage": "CSPFT_INT"}

    def test_item_with_own_melee_swings_itself_like_bo2_riotshield(self):
        rake = weapons.convert("nt_rake_trap", {"inventoryType": "item", "meleeAnim": "rake_melee",
                                                "meleeDamage": "2500"}, self.T4, self.T4)
        self.assertEqual(rake.fields["useAsMelee"], "1")

    def test_guns_keep_the_bo2_melee_weapon(self):
        gun = weapons.convert("gun", {"inventoryType": "primary", "meleeAnim": "gun_melee",
                                      "meleeDamage": "150"}, self.T4, self.T4)
        self.assertNotIn("useAsMelee", gun.fields)

    def test_scripted_item_idle_placeholder_is_not_a_melee_attack(self):
        for idle_field in ("idleAnim", "emptyIdleAnim"):
            with self.subTest(idle_field=idle_field):
                item = weapons.convert("assembly_animation", {
                    "inventoryType": "item", "weaponType": "grenade",
                    idle_field: "hands_idle", "meleeAnim": "hands_idle",
                    "meleeDamage": "40"}, self.T4, self.T4)
                self.assertNotIn("useAsMelee", item.fields)

    def test_explicit_melee_setting_is_preserved(self):
        schema = {**self.T4, "useAsMelee": "CSPFT_BOOL"}
        item = weapons.convert("item", {"inventoryType": "item",
            "idleAnim": "idle", "meleeAnim": "attack", "meleeDamage": "40",
            "useAsMelee": "0"}, schema, schema)
        self.assertEqual(item.fields["useAsMelee"], "0")


class EngineMaterialTests(unittest.TestCase):
    def test_special_unlit_set_uses_the_same_named_t6_set(self):
        from waw2bo2 import techsets
        m = techsets.match("flamethrowerfx_color_distort_overlay_bloom",
                           ["flamethrowerfx_color_distort_overlay_bloom_7287q544", "distortion_81587199"])
        self.assertEqual(m.target, "flamethrowerfx_color_distort_overlay_bloom_7287q544")

    def test_setelectrified_draws_the_shock_overlay(self):
        # both executables draw this material by name for SetElectrified
        self.assertEqual(t6bridge.ENGINE_BUILTIN_MATERIALS["setelectrified"], "zombie_electric_shock_overlay")

    def test_waw_bowie_is_ported_and_its_perk_equips_the_t6_melee_weapon(self):
        # WaW's _zombiemode_bowie runs (buy, flourish, dialog); BO2's melee
        # wallbuy is not set up on the same triggers; the engine perk it sets
        # becomes T6's melee weapon
        self.assertIn("maps\\_zombiemode_bowie", gscport.API["preserved_scripts"])
        self.assertNotIn("maps\\_zombiemode_bowie", gscport.API["core_scripts"])
        compat = (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8")
        script = gscport.gsc.parse(compat, "compat")
        self.assertIn("waw_altmelee_weapon", script.functions)
        self.assertNotIn("melee_wallbuy_costs", script.functions)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            main = root / "maps/mp/zm_x.gsc"
            main.parent.mkdir(parents=True)
            main.write_text("main()\n{\n    maps\\mp\\zombies\\_zm_weap_bowie::init();\n    level.x = 1;\n}\n")
            self.assertEqual(gscport.drop_preserved_template_inits(main, root), [])    # map does not port it
            ported = root / "maps/mp/waw/_zombiemode_bowie.gsc"
            ported.parent.mkdir(parents=True)
            ported.write_text("bowie_init()\n{\n}\n")
            self.assertEqual(gscport.drop_preserved_template_inits(main, root),
                             ["maps\\mp\\zombies\\_zm_weap_bowie::init();"])
            self.assertNotIn("_zm_weap_bowie", main.read_text())

    def test_boarded_windows_are_shut_to_path_finding(self):
        # WaW's clip brush disconnects a boarded window's traversal; T6's
        # blocker_(dis)connect_paths are empty, so find_flesh zombies walked
        # through six closed boards (Shi No Numa comm room, measured).
        script = gscport.gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"), "compat")
        fn = script.functions["waw_window_paths"]
        body = gscport.gsc.emit(script.tokens[fn.body_open:fn.body_close + 1])
        self.assertIn("all_chunks_destroyed( goal, goal.barrier_chunks )", body)
        # unlinknodes leaves a T6 negotiation link in place (measured)
        self.assertIn("setenablenode( goal.neg_end, open )", body)
        self.assertNotIn("unlink", body)
        init = script.functions["init"]
        self.assertIn("level thread waw_window_paths();",
                      gscport.gsc.emit(script.tokens[init.body_open:init.body_close + 1]))


class FrameworkSignatureTests(unittest.TestCase):
    def test_a_maps_own_function_runs_when_bo2s_takes_other_arguments(self):
        # Shi No Numa's _zombiemode_net: network_safe_spawn( classname, origin );
        # BO2's _zm_net (and Der Riese's WaW) take ( id, max, classname, origin )
        self.assertEqual(gscport.BO2_PARAMS["maps\\mp\\zombies\\_zm_net"]["network_safe_spawn"],
                         ["id", "max", "classname", "origin"])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode_net.gsc").write_text(
                "network_safe_spawn( classname, origin ) { return Spawn( classname, origin ); }\n"
                "network_choke_init( id ) { }\n")
            # Der Riese's WaW takes BO2's ( id, max, classname, origin ): BO2's runs
            (root / "maps/_zombiemode_utility.gsc").write_text(
                "all_chunks_intact( barrier_chunks ) { }\nflag_init( message ) { }\n")
            tr = gscport.Translator(gscport.Sources([root], [], None), T6Api({}, {}), gscport.PortReport())
            self.assertTrue(tr.signature_differs("maps\\_zombiemode_net", "network_safe_spawn"))
            self.assertFalse(tr.signature_differs("maps\\_zombiemode_net", "network_choke_init"))
            self.assertFalse(tr.signature_differs("maps\\_zombiemode_net", "not_in_bo2"))
            # barrier helpers keep BO2's routing and its argument adapters
            self.assertFalse(tr.signature_differs("maps\\_zombiemode_utility", "all_chunks_intact"))

    def test_waws_magic_bullet_shield_runs(self):
        # BO2's only lowers attacker accuracy: dogs died during their spawn
        # lightning and the dog script carried on with a dead dog
        self.assertTrue(gscport.source_drives("maps\\_utility", "magic_bullet_shield"))
        self.assertTrue(gscport.source_drives("maps\\_utility", "stop_magic_bullet_shield"))
        self.assertFalse(gscport.source_drives("maps\\_utility", "array_thread"))
        # its BloodImpact( bool ) takes T6's mode names, else the shield's
        # thread ends before its health loop
        self.assertIn("bloodimpact", gscport.API["compat_wrappers"])
        compat = gscport.gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"), "compat")
        body = gscport.gsc.emit(compat.tokens[compat.functions["waw_bloodimpact"].body_open:
                                              compat.functions["waw_bloodimpact"].body_close + 1])
        self.assertIn('"none"', body)
        self.assertIn('"normal"', body)

    def test_player_teleports_are_not_interpolated(self):
        # Der Riese's teleporter: link to a script_origin, move it to the
        # black room, setorigin to the core; T6's client slid the view there
        self.assertIn("linkto", gscport.API["compat_wrappers"])
        self.assertIn("setorigin", gscport.API["compat_wrappers"])
        compat = gscport.gsc.parse((gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8"), "compat")
        for name in ("waw_linkto", "waw_setorigin"):
            body = gscport.gsc.emit(compat.tokens[compat.functions[name].body_open:
                                                  compat.functions[name].body_close + 1])
            self.assertIn("self dontinterpolate();", body)

    def test_the_maps_net_init_runs_where_bo2_has_none(self):
        # its network_safe_spawn reads the counters init sets
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "maps").mkdir()
            (root / "maps/_zombiemode_net.gsc").write_text(
                "init() { level.zombie_network_num_spawns = 0; }\n"
                "network_safe_spawn( classname, origin ) { return Spawn( classname, origin ); }\n")
            (root / "maps/_zombiemode.gsc").write_text("main() { maps\\_zombiemode_net::init(); }\n")
            api = T6Api({}, {"maps\\mp\\zombies\\_zm_net": {"network_safe_spawn": 4}})
            tr = gscport.Translator(gscport.Sources([root], [], None), api, gscport.PortReport())
            self.assertEqual(tr.framework_hooks(), ["maps\\mp\\waw\\_waw2bo2_core::zombiemode_net__init();"])


if __name__ == "__main__":
    unittest.main()

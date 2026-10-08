import tempfile
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
        self.assertIn("return iDamage;", core)            # fall through = damage unchanged
        self.assertNotIn("MOD_FALLING", core)             # BO2 _zm keeps the stock damage logic
        self.assertIn("register_player_damage_callback( ::zombiemode__player_damage_override_prelude )", core)
        self.assertIn("\tzombiemode__player_damage_override_prelude_register();", core)
        self.assertFalse([e for e in report.errors if "damage" in e])

    def test_map_edited_from_the_raw_release_matches_it_not_the_zone_one(self):
        # stock zones carry an older _zombiemode than the raw scripts maps start from
        older = STOCK_DAMAGE.replace("\tif( sMeansOfDeath",
                                     "\tif( iDamage < self.health )\n\t{\n\t\treturn;\n\t}\n\tif( sMeansOfDeath")
        body = STOCK_DAMAGE.split("{\n", 1)
        core, report = self.port(body[0] + "{\n\tif( self.shielded )\n\t\treturn;\n" + body[1], older)
        self.assertIn("if( self.shielded )\n\t\treturn 0;", core)
        self.assertFalse([e for e in report.errors if "damage" in e])

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
        self.assertFalse([e for e in report.errors if 'damage' in e])

    def test_statement_split_keeps_if_else_chains_whole(self):
        tokens = gscport.gsc.tokenize("{ if ( a ) { b(); } else c(); d = 1; for ( i = 0; i < 2; i++ ) e(); }")
        self.assertEqual(len(gscport.top_level_statements(tokens, 0, len(tokens) - 2)), 3)

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
        self.assertFalse([e for e in report.errors if 'damage' in e])

    def test_positive_damage_finish_is_still_rejected(self):
        body = STOCK_DAMAGE.split("{\n", 1)
        prelude = '''if (self.custom_damage) {
            self maps\\_callbackglobal::finishPlayerDamageWrapper(eInflictor, eAttacker, 75,
                iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, modelIndex, psOffsetTime);
            return;
        }\n'''
        _, report = self.port(body[0] + '{\n' + prelude + body[1])
        self.assertTrue([e for e in report.errors if 'prelude applies damage itself' in e])


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


class EngineMaterialTests(unittest.TestCase):
    def test_special_unlit_set_uses_the_same_named_t6_set(self):
        from waw2bo2 import techsets
        m = techsets.match("flamethrowerfx_color_distort_overlay_bloom",
                           ["flamethrowerfx_color_distort_overlay_bloom_7287q544", "distortion_81587199"])
        self.assertEqual(m.target, "flamethrowerfx_color_distort_overlay_bloom_7287q544")

    def test_setelectrified_draws_the_shock_overlay(self):
        # both executables draw this material by name for SetElectrified
        self.assertEqual(t6bridge.ENGINE_BUILTIN_MATERIALS["setelectrified"], "zombie_electric_shock_overlay")
        compat = (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8")
        # BO2 melee wallbuys charge self.stub.cost; WaW trigger_use wallbuys get one
        self.assertIn("triggers[j].stub.cost = melee_weapon.cost;", compat)


if __name__ == "__main__":
    unittest.main()

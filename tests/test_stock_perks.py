import json
from pathlib import Path
import tempfile
import unittest

from waw2bo2 import gsc, gscport, stockperks
from waw2bo2.launcher import Settings, BuildPaths
from waw2bo2.t6api import T6Api


class StockPerkTests(unittest.TestCase):
    def test_option_defaults_off_and_survives_settings_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings.json'
            self.assertFalse(Settings().bo2_stock_perks)
            Settings(bo2_stock_perks=True).save(path)
            self.assertTrue(Settings.load(path).bo2_stock_perks)
            path.write_text('{"bo2_stock_perks":"false"}')
            self.assertFalse(Settings.load(path).bo2_stock_perks)

    def test_changing_perk_mode_requires_a_matching_build_before_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings = Settings(work=tmp, project='zm_sample', fastfile=str(Path(tmp) / 'sample.ff'))
            paths = BuildPaths.for_settings(settings)
            paths.output.mkdir(parents=True)
            for name in ('zm_sample.ff', 'zm_sample.ipak', 'mod.ff', 'mod_load.ff', 'mod.json'):
                (paths.output / name).touch()
            receipt = paths.root / 'build.json'
            receipt.write_text(json.dumps({'project': settings.project}))
            self.assertTrue(paths.complete(settings.project, bo2_stock_perks=False))
            self.assertFalse(paths.complete(settings.project, bo2_stock_perks=True))
            receipt.write_text(json.dumps({'project': settings.project, 'bo2_stock_perks': True}))
            self.assertTrue(paths.complete(settings.project, bo2_stock_perks=True))
            self.assertFalse(paths.complete(settings.project, bo2_stock_perks=False))

    def test_native_purchase_loss_and_client_hud_code_remain_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for prefix, ext in (('maps', 'gsc'), ('clientscripts', 'csc')):
                for module in stockperks.MODULES:
                    path = root / f'bo2/raw/{prefix}/mp/zombies/_zm_perk_{module}.{ext}'
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(f'enable_{module}_perk_for_level() {{}}')
                original = 'init() { start_native_purchases(); }\n'
                original += 'perk_think(perk) { wait_for_down(); lose_native_perk(perk); }\n'
                original += 'perk_init_code_callbacks() { bind_native_hud(); }\n'
                output = stockperks.native_source(original, f'{prefix}/mp/zombies/_zm_perks.{ext}',
                                                  root / 'out', root / 'bo2')
                script = gsc.parse(output, 'native')
                self.assertIn('start_native_purchases();', output)
                for name in ('perk_think', 'perk_init_code_callbacks'):
                    before = gsc.parse(original, 'original')
                    fn = before.functions[name]
                    actual = script.functions[name]
                    self.assertEqual(gsc.emit(before.tokens[fn.body_open:fn.body_close + 1]),
                                     gsc.emit(script.tokens[actual.body_open:actual.body_close + 1]))
                self.assertIn('enable_electric_cherry_perk_for_level();', output)

    def test_entities_and_script_comparisons_use_same_native_perk_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'entities.json'
            path.write_text(json.dumps({'entities': [{'targetname': 'zombie_vending_bo1',
                'script_noteworthy': 'specialty_extraammo', 'target': 'vending_mule_kick'}]}))
            stockperks.translate_entities(path)
            entity = json.loads(path.read_text())['entities'][0]
            self.assertEqual(entity, {'targetname': 'zombie_vending',
                'script_noteworthy': 'specialty_additionalprimaryweapon',
                'target': 'vending_additionalprimaryweapon'})
            source = stockperks.translate_literals('if (perk == "specialty_extraammo") {} '
                'level notify("specialty_extraammo_power_on"); // specialty_extraammo\n'
                'hint = &"ZOMBIE_PERK_QUICKREVIVE";')
            self.assertIn('"specialty_additionalprimaryweapon"', source)
            self.assertIn('"specialty_additionalprimaryweapon_power_on"', source)
            self.assertIn('// specialty_extraammo', source)
            self.assertIn('&"ZOMBIE_PERK_QUICKREVIVE"', source)

    def test_custom_perk_machine_rejects_stock_mode_without_rewriting_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'entities.json'
            original = json.dumps({'entities': [{'targetname': 'zombie_vending_bo1',
                                                  'script_noteworthy': 'specialty_custom'}]})
            path.write_text(original)
            with self.assertRaisesRegex(ValueError, 'unsupported perk'):
                stockperks.translate_entities(path)
            self.assertEqual(path.read_text(), original)

    def test_stock_mode_never_starts_source_perk_controllers_or_duplicate_hud(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {'maps/sample.gsc': '#include maps\\_zombiemode_perks;\n'
                'main() {\n maps\\_zombiemode::main();\n self setperk("specialty_quickrevive");\n'
                'self thread perk_hud_create("specialty_quickrevive");\n'
                'self thread perk_think("specialty_quickrevive");\n }',
                'maps/_zombiemode.gsc': 'main() { maps\\_zombiemode_perks::init(); '
                'maps\\_zombiemode_perks_extra::init(); }',
                'maps/_zombiemode_perks.gsc': 'init() {} perk_hud_create(perk) {} perk_think(perk) {}',
                'maps/_zombiemode_perks_extra.gsc': 'init() { start_custom_purchase(); }'}
            for name, source in files.items():
                path = root / 'source' / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
            report = gscport.port_map(gscport.Sources([root / 'source'], [], root / 'source'),
                T6Api({}, {r'maps\mp\zombies\_zm_perks': {'perk_think': 1}}),
                'sample', root / 'out', bo2_stock_perks=True)
            self.assertFalse(any(stockperks.is_perk_framework(p) for p in report.ported))
            self.assertFalse(any('stock BO2 perks' in error for error in report.errors))
            source = (root / 'out/maps/mp/waw/sample.gsc').read_text()
            self.assertEqual(source.count('stock_perks_owned_by_bo2('), 2)
            self.assertNotIn('::perk_think(', source)
            self.assertIn('::waw_setperk(', source)

    def test_hidden_box_trigger_is_restored_without_clearing_buyer_visibility(self):
        source = (gscport.COMPAT_DIR / '_waw2bo2_compat.gsc').read_text()
        script = gsc.parse(source, 'compat')
        fn = script.functions['waw_enable_trigger']
        body = gsc.emit(script.tokens[fn.body_open:fn.body_close + 1])
        self.assertLess(body.index('::enable_trigger()'), body.index('self show()'))
        self.assertIn('self.classname == "trigger_use"', body)
        self.assertNotIn('setvisibletoall', body)
        self.assertEqual(gscport.API['function_renames']['enable_trigger'],
                         gscport.COMPAT + '::waw_enable_trigger')

    def test_source_box_move_flags_are_isolated_for_initialization_and_waits(self):
        tokens = gsc.tokenize('flag_init("moving_chest_now"); flag_set("moving_chest_now"); '
            'if (flag("moving_chest_now")) {} flag_clear("moving_chest_now"); '
            'flag_wait("power_on"); level.chests = [];')
        gscport.rename_level_fields(tokens)
        text = gsc.emit(tokens)
        self.assertEqual(text.count('"waw_moving_chest_now"'), 4)
        self.assertIn('flag_wait("power_on")', text)
        self.assertIn('level.waw_chests', text)


if __name__ == '__main__':
    unittest.main()

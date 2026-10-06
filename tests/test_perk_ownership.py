import tempfile
import unittest
from pathlib import Path

from waw2bo2 import gsc, gscport, t6bridge
from waw2bo2.t6api import T6Api


class PerkOwnershipTests(unittest.TestCase):
    def write_client_stock(self, root):
        path = root / 'bo2/raw/clientscripts/mp/zombies/_zm_perks.csc'
        path.parent.mkdir(parents=True)
        path.write_text('init() { init_custom_perks(); perks_register_clientfield(); '
                        'init_perk_custom_threads(); }\n'
                        'init_custom_perks() {}\n'
                        'perks_register_clientfield() {\n'
                        ' registerclientfield("toplayer", "perk_quick_revive", 1, 2, '
                        '"int", level.zombies_global_perk_client_callback, 0, 1);\n'
                        ' registerclientfield("scriptmover", "clientfield_perk_intro_fx", '
                        '1000, 1, "int", ::perk_meteor_fx, 0);\n'
                        ' level thread perk_init_code_callbacks();\n }\n'
                        'perk_init_code_callbacks() { wait 0.1; '
                        'setupclientfieldcodecallbacks("toplayer", 1, "perk_quick_revive"); }\n'
                        'init_perk_custom_threads() { level thread custom_behavior(); }\n'
                        'perk_meteor_fx() { playfx(); }\n')
        (root / 'bo2/raw/clientscripts/mp/zombies/_zm.csc').write_text(
            '#include clientscripts\\mp\\zombies\\_zm_perks;\n'
            'init() { clientscripts\\mp\\zombies\\_zm_perks::init(); '
            'clientscripts\\mp\\zombies\\_zm_score::init(); }\n')
        return path

    def test_stock_waw_perks_are_ported_and_started_once_with_extensions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {
                'maps/sample.gsc': 'main() {\n maps\\_zombiemode::main();\n }',
                'maps/_zombiemode.gsc': 'main() { maps\\_zombiemode_perks::init(); '
                    'maps\\_zombiemode_perks_extra::init(); }',
                'maps/_zombiemode_perks.gsc': 'init() {} perk_think(perk) {}',
                'maps/_zombiemode_perks_extra.gsc': 'init() { '
                    'self maps\\_zombiemode_perks::perk_think("custom"); }',
            }
            for name, source in files.items():
                p = root / ('map' if name.endswith('_extra.gsc') else 'stock') / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(source)
            sources = gscport.Sources([root / 'map'], [], root / 'stock')
            self.assertFalse(gscport.is_core('maps\\_zombiemode_perks', sources))
            api = T6Api({}, {'maps\\mp\\zombies\\_zm_perks': {'init': 0, 'perk_think': 1}})
            report = gscport.port_map(sources, api, 'sample', root / 'out')
            self.assertIn('maps\\_zombiemode_perks', report.ported)
            base = root / 'out/maps/mp/waw'
            main = (base / 'sample.gsc').read_text()
            self.assertEqual(main.count('maps\\mp\\waw\\_zombiemode_perks::init();'), 1)
            extra = (base / '_zombiemode_perks_extra.gsc').read_text()
            self.assertIn('maps\\mp\\waw\\_zombiemode_perks::perk_think', extra)
            self.assertNotIn('maps\\mp\\zombies\\_zm_perks::', main + extra)

    def test_source_effect_table_cannot_be_overwritten_by_bo2(self):
        tokens = gsc.tokenize('level._effect = []; level._effect["packapunch_fx"] = '
                              'loadfx("source"); playfx(level._effect["packapunch_fx"], origin); '
                              'ent._effect = 1; note = "level._effect";')
        gscport.rename_level_fields(tokens)
        text = gsc.emit(tokens)
        self.assertEqual(text.count('level.waw_effect'), 3)
        self.assertIn('ent._effect = 1;', text)
        self.assertIn('"level._effect"', text)

    def test_bo2_machine_controllers_are_removed_but_runtime_exports_remain(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            name = gscport.BO2_PERK_OVERRIDES[0]
            stock = root / 'bo2/raw' / name
            stock.parent.mkdir(parents=True)
            stock.write_text('init() { perk_machine_spawn_init(); array_thread(x,::buy); }\n'
                             'perk_pause_all_perks() { pause_machine(); }\n'
                             'perk_unpause_all_perks() { unpause_machine(); }\n'
                             'perks_register_clientfield() { registerclientfield("x"); }\n'
                             'initialize_custom_perk_arrays() {}\n'
                             'support_function(a) { return a; }\n')
            (root / 'bo2/raw/maps/mp/zombies/_zm.gsc').write_text(
                '#include maps\\mp\\zombies\\_zm_perks;\n'
                'init() { maps\\mp\\zombies\\_zm_perks::init(); '
                'maps\\mp\\zombies\\_zm_score::init(); }\n')
            out = root / 'out'
            client_stock = self.write_client_stock(root)
            gscport.stage_bo2_perk_support(out, root / 'bo2')
            text = (out / name).read_text()
            self.assertNotIn('perk_machine_spawn_init();', text)
            self.assertNotIn('array_thread(x,::buy)', text)
            self.assertNotIn('unpause_machine();', text)
            self.assertIn('perks_register_clientfield();', text)
            self.assertIn('support_function(a) { return a; }', text)
            self.assertIn(name, t6bridge.map_scripts(out, 'any_project'))
            parsed = gsc.parse(text, name)
            self.assertIn('support_function', parsed.functions)
            client_name = client_stock.relative_to(root / 'bo2/raw').as_posix()
            client_text = (out / client_name).read_text()
            client = gsc.parse(client_text, client_name)
            original_client = gsc.parse(client_stock.read_text(), client_name)
            def body(script, name):
                fn = script.functions[name]
                return gsc.emit(script.tokens[fn.body_open:fn.body_close + 1])
            # Preserve field names, order, versions, bit widths and FX callbacks.
            self.assertEqual(body(client, 'perks_register_clientfield'),
                             body(original_client, 'perks_register_clientfield'))
            self.assertEqual(body(client, 'perk_meteor_fx'),
                             body(original_client, 'perk_meteor_fx'))
            self.assertFalse(gsc.references(client.tokens,
                             client.functions['perk_init_code_callbacks'].body_open,
                             client.functions['perk_init_code_callbacks'].body_close))
            self.assertNotIn('init_perk_custom_threads();', body(client, 'init'))
            self.assertNotIn('custom_behavior();', client_text)
            self.assertIn(client_name, t6bridge.map_scripts(out, 'any_project'))
            owned_name = 'clientscripts/mp/waw/_waw2bo2_perks.csc'
            self.assertEqual((out / owned_name).read_text(), client_text)
            bootstrap_name = 'clientscripts/mp/waw/_waw2bo2_zm.csc'
            bootstrap = (out / bootstrap_name).read_text()
            self.assertIn('clientscripts\\mp\\waw\\_waw2bo2_perks::init()', bootstrap)
            self.assertNotIn('clientscripts\\mp\\zombies\\_zm_perks', bootstrap)
            self.assertIn('clientscripts\\mp\\zombies\\_zm_score::init()', bootstrap)
            owned_server = out / 'maps/mp/waw/_waw2bo2_perks.gsc'
            self.assertNotIn('perk_machine_spawn_init();', owned_server.read_text())
            bootstrap_server = out / 'maps/mp/waw/_waw2bo2_zm.gsc'
            self.assertIn('maps\\mp\\waw\\_waw2bo2_perks::init()', bootstrap_server.read_text())
            self.assertNotIn('maps\\mp\\zombies\\_zm_perks', bootstrap_server.read_text())
            server_main = out / 'maps/mp/any_project.gsc'
            server_main.write_text('main() { maps\\mp\\zombies\\_zm::init(); '
                                   'level thread maps\\mp\\zombies\\_zm_perks::perk_unpause_all_perks(); }')
            gscport.hook_bo2_perk_server(server_main)
            hooked_server = server_main.read_text()
            self.assertIn('maps\\mp\\waw\\_waw2bo2_zm::init()', hooked_server)
            self.assertNotIn('maps\\mp\\zombies\\_zm_perks::', hooked_server)
            gscport.hook_bo2_perk_server(server_main)
            self.assertEqual(hooked_server, server_main.read_text())
            main = out / 'clientscripts/mp/any_project.csc'
            main.write_text('main() { clientscripts\\mp\\zombies\\_zm::init(); }')
            gscport.hook_bo2_perk_client(main)
            hooked = main.read_text()
            self.assertIn('clientscripts\\mp\\waw\\_waw2bo2_zm::init()', hooked)
            self.assertNotIn('clientscripts\\mp\\zombies\\_zm::init()', hooked)
            gscport.hook_bo2_perk_client(main)
            self.assertEqual(main.read_text(), hooked)
            self.assertIn(owned_name, t6bridge.map_scripts(out, 'any_project'))
            self.assertIn(bootstrap_name, t6bridge.map_scripts(out, 'any_project'))
            gscport.stage_bo2_perk_support(out, root / 'bo2')
            self.assertEqual((out / name).read_text(), text)
            self.assertEqual((out / client_name).read_text(), client_text)
            self.assertIn('perk_machine_spawn_init();', stock.read_text())


if __name__ == '__main__':
    unittest.main()

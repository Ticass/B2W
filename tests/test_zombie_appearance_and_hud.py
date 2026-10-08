import json
from pathlib import Path
import tempfile
import unittest

from waw2bo2 import gsc, gscport, zombieappearance, t6bridge, entities


class ZombieAppearanceAndHudTests(unittest.TestCase):
    def test_unrotated_camera_structs_have_native_vector_defaults(self):
        source = [{'classname': 'script_struct', 'targetname': 'camera_end', 'origin': '8 9 10'},
                  {'classname': 'script_struct', 'angle': '45'},
                  {'classname': 'script_struct', 'angles': '10 20 30'}]
        converted, _, _ = entities.convert_entities(source, 'sample')
        converted = converted[1:]  # generated worldspawn
        self.assertEqual(converted[0]['angles'], '0 0 0')
        self.assertEqual(converted[0]['origin'], '8 9 10')
        self.assertEqual(converted[1]['angles'], '0 45 0')
        self.assertEqual(converted[2]['angles'], '10 20 30')
        self.assertNotIn('angles', source[0])

    def test_animated_models_keep_all_lods_and_use_dynamic_t6_renderer(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'source'
            output = Path(folder) / 'output'
            (source / 'xmodel').mkdir(parents=True)
            lods = [{'file': 'high.gltf', 'distance': 250},
                    {'file': 'low.gltf', 'distance': 1000000}]
            for lod in lods:
                (source / lod['file']).write_text('{"nodes":[],"materials":[]}')
            for name, kind in [('actor_body', 'animated'), ('prop', 'rigid')]:
                (source / f'xmodel/{name}.json').write_text(json.dumps({'type': kind, 'lods': lods}))
            report = t6bridge.StageReport('sample')
            with patch.object(t6bridge, 'OAT_T6_RAW', source):
                (source / 'partclassification.csv').write_text('')
                t6bridge.stage_models(report, SimpleNamespace(static_models=[]), 'sample', source,
                                      output, {'actor_body', 'prop'}, [source])
            self.assertEqual(report.errors, [])
            body = json.loads((output / 'xmodel/actor_body.json').read_text())
            prop = json.loads((output / 'xmodel/prop.json').read_text())
            self.assertEqual(body['flags'], 0x80000)
            self.assertEqual(prop['flags'], 0x200000)
            self.assertEqual(body['lods'], lods)

    def test_wall_buys_without_models_do_not_bind_unrelated_objects(self):
        from collections import Counter
        data = [{'targetname': 'weapon_upgrade'},
                {'targetname': 'waw_wallbuy_without_model_0'},
                {'targetname': 'weapon_upgrade', 'target': 'authored_display'}]
        counts = Counter()
        entities.wall_buy_null_targets(data, counts)
        self.assertNotIn(data[0]['target'], {e['targetname'] for e in data})
        self.assertEqual(data[2]['target'], 'authored_display')
        self.assertEqual(sum(counts.values()), 1)

    def test_distinct_source_actor_types_keep_distinct_characters(self):
        template = '''#include character\\donor;
        main() { self.animtree = "native";
          randchar = codescripts\\character::get_random_character(2);
          switch(randchar) { case 0: character\\donor::main(); break; }
          self setcharacterindex(randchar);
        }
        precache(ai_index) { precacheanimstatedef(ai_index, 0, "native");
          character\\donor::precache(); }
        spawner() { self setspawnerteam("axis"); }'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'BSP').mkdir()
            entities = [{'classname': 'actor_donor', 'waw_actor_type': name,
                         'script_char_index': str(i)} for i, name in enumerate(('zombie_custom', 'zombie_stock'))]
            (root / 'BSP/entities.json').write_text(json.dumps({'entities': entities}))
            zombieappearance.bind_actor_types(root, zombieappearance.source_actor_types(root))
            bound = json.loads((root / 'BSP/entities.json').read_text())['entities']
            self.assertEqual([e['classname'] for e in bound], ['actor_waw_zombie_custom', 'actor_waw_zombie_stock'])
            self.assertEqual([e['script_char_index'] for e in bound], ['0', '1'])
            for name in ('zombie_custom', 'zombie_stock'):
                native = zombieappearance.native_actor(template, gscport.ported_path('aitype\\' + name))
                self.assertNotIn('character\\donor', native)
                self.assertIn('self.animtree = "native";', native)
                self.assertIn('::precache();', native)
                self.assertIn(name + '::main();', native)
                (root / 'aitype').mkdir(exist_ok=True)
                (root / 'aitype' / ('waw_' + name + '.gsc')).write_text(native)
            self.assertIn('aitype/waw_zombie_custom.gsc', t6bridge.map_scripts(root, 'sample'))

    def test_source_actor_selection_and_custom_fields_survive_engine_translation(self):
        source = '''main() { self.type = "human"; self.weapon = "kar98k";
          self.custom_state = 7;
          switch(codescripts\\character::get_random_character(2)) {
            case 0: character\\custom_body::main(); break;
            case 1: character\\stock_body::main(); break;
          }
        }
        precache() { character\\custom_body::precache(); precacheitem("kar98k"); }'''
        result = zombieappearance.appearance_script(source)
        self.assertNotIn('self.type', result)
        self.assertNotIn('kar98k', result)
        self.assertIn('self.custom_state = 7;', result)
        self.assertIn('character\\custom_body::main()', result)
        self.assertIn('character\\stock_body::main()', result)
        self.assertIn('get_random_character(2)', result)

    def test_waw_network_scale_wraps_but_ordinary_sizes_survive(self):
        tokens = gsc.tokenize('f(){ hud.fontscale = 8.0; small.fontscale = 1.6; '
                              'calculated.fontscale = base + offset; label = "fontscale = 8.0"; }')
        self.assertEqual(gscport.bridge_font_scales(tokens), 2)
        result = gsc.emit(tokens)
        self.assertIn('hud.fontscale = 1.6;', result)
        self.assertIn('small.fontscale = 1.6;', result)
        self.assertIn('::waw_fontscale( base + offset )', result)
        self.assertIn('label = "fontscale = 8.0";', result)

    def test_quick_revive_registers_a_solo_life_without_duplicate_stock_grant(self):
        source = (gscport.COMPAT_DIR / '_waw2bo2_compat.gsc').read_text()
        script = gsc.parse(source, 'compat')
        fn = script.functions['waw_setperk']
        body = gsc.emit(script.tokens[fn.body_open:fn.body_close + 1])
        self.assertIn('getnumconnectedplayers() == 1', body)
        self.assertIn('perk == "specialty_quickrevive"', body)
        self.assertIn('self.lives = 1;', body)
        self.assertLess(body.index('::give_perk'), body.index('self setperk'))
        self.assertIn('return;', body[body.index('::give_perk'):body.index('self setperk')])

    def test_actor_scripts_can_resolve_stock_mod_tools_sources_after_map_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for folder, body in (('map', 'custom_body'), ('raw', 'stock_body')):
                path = root / folder / 'character/zombie.gsc'
                path.parent.mkdir(parents=True)
                path.write_text(f'main() {{ self setmodel("{body}"); }}')
            sources = gscport.Sources([root / 'map'], [], None, [root / 'raw'])
            self.assertIn('custom_body', gsc.emit(sources.get('character/zombie').tokens))
            sources = gscport.Sources([], [], None, [root / 'raw'])
            self.assertIn('stock_body', gsc.emit(sources.get('character/zombie').tokens))


if __name__ == '__main__':
    unittest.main()

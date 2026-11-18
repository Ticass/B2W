import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import gsc, hintdefaults, materialisolation, scorebehavior


class SourceScoreTests(unittest.TestCase):
    def test_perk_arithmetic_keeps_authored_event_and_multiplier(self):
        source = '''player_add_points(event,mod,hit_location,is_dog) {
          switch(event) {case "death": points=50;
            if(self hasperk("custom_score_perk")) { points=points*3; }
            break; case "damage": points=10; break;} self.score+=points;
        }'''
        rules = scorebehavior.source_rules(gsc.parse(source, 'source'))
        self.assertIn('event == "death"', rules)
        self.assertIn('points=points*3;', rules)
        self.assertNotIn('points=50;', rules)
        self.assertNotIn('self.score', rules)
        self.assertIsNone(scorebehavior.source_rules(gsc.parse(
            source.replace('points=points*3;', 'self notify("changed");'), 'source')))
        self.assertIsNone(scorebehavior.source_rules(gsc.parse(
            source.replace('self hasperk("custom_score_perk")', 'local_state && self hasperk("custom_score_perk")'), 'source')))
        self.assertIsNone(scorebehavior.source_rules(gsc.parse(
            source.replace('points=points*3;', 'points=points();'), 'source')))
        self.assertIsNone(scorebehavior.source_rules(gsc.parse(
            source.replace('if(self hasperk', 'if(local_state) { if(self hasperk').replace('break; case', '} break; case'), 'source')))

    def test_native_scoring_preserves_multiplier_order_and_routes_owned_callbacks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            rules = root / 'maps/mp/waw/_waw2bo2_score_rules.gsc'
            rules.parent.mkdir(parents=True)
            rules.write_text('adjust(points,event,mod,hit_location,is_dog){return points;}')
            raw = root / 'bo2/raw/maps/mp/zombies'
            raw.mkdir(parents=True)
            (raw / '_zm_score.gsc').write_text('''player_add_points(event,mod,hit_location,is_dog) {
                player_points=50; player_points = multiplier * round_up_score(player_points,5);
                self native_score_effect(player_points); }''')
            (raw / '_zm_spawner.gsc').write_text(
                'death(){self maps\\mp\\zombies\\_zm_score::player_add_points("death");}')
            boot = rules.parent / '_waw2bo2_zm.gsc'
            boot.write_text('init(){level.spawn_init=maps\\mp\\zombies\\_zm_spawner::death;}')
            scorebehavior.stage_native(root, root / 'bo2')
            text = (rules.parent / '_waw2bo2_score.gsc').read_text()
            self.assertLess(text.index('::adjust('), text.index('multiplier *'))
            self.assertIn('native_score_effect', text)
            self.assertIn('_waw2bo2_spawner::death', boot.read_text())
            self.assertIn('_waw2bo2_score::player_add_points', (rules.parent / '_waw2bo2_spawner.gsc').read_text())


class MaterialIsolationTests(unittest.TestCase):
    def test_cached_material_collision_renames_bindings_and_preserves_pixels(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project, stock = root / 'project', root / 'stock'
            relative = Path('materials/mc/shared.json')
            for base in (project, stock):
                (base / relative).parent.mkdir(parents=True)
            material = {'techniqueSet': 'waw/runtime_source', 'textures': [{'image': 'waw_world/skin'}]}
            (project / relative).write_text(json.dumps(material))
            (stock / relative).write_text('{}')
            gltf = project / 'model_export/model.gltf'
            gltf.parent.mkdir()
            cached = root / 'cached.gltf'
            cached.write_text(json.dumps({'materials': [{'name': 'mc/shared'}]}))
            gltf.hardlink_to(cached)
            fbx = project / 'BSP/map_gfx.fbx'
            fbx.parent.mkdir()
            fbx.write_text('Material::mc/shared" Material::mc/shared_extra"')
            (project / 'BSP/lightmaps.json').write_text(json.dumps({'wawMaterials': ['mc/shared', 'unchanged']}))
            (project / 'BSP/constraints.json').write_text(json.dumps({'constraints': [{'material': 'mc/shared'}, {'material': ''}]}))
            script = project / 'maps/mp/test.gsc'
            script.parent.mkdir(parents=True)
            script.write_text('init(){material="mc/shared";self setshader(material);}')
            report = SimpleNamespace(materials=[{'name': 'mc/shared', 'file': str(relative)}],
                                     scripts={'hud_materials': ['mc/shared']}, warnings=[])
            mapping = materialisolation.stage(report, project, stock)
            self.assertEqual(mapping, {'mc/shared': 'waw_world/mc/shared'})
            self.assertEqual(json.loads((project / report.materials[0]['file']).read_text()), material)
            self.assertEqual(json.loads(gltf.read_text())['materials'][0]['name'], 'waw_world/mc/shared')
            self.assertEqual(json.loads(cached.read_text())['materials'][0]['name'], 'mc/shared')
            self.assertEqual(fbx.read_text(), 'Material::waw_world/mc/shared" Material::mc/shared_extra"')
            self.assertIn('material="waw_world/mc/shared"', script.read_text())
            self.assertEqual(json.loads((project / 'BSP/lightmaps.json').read_text())['wawMaterials'],
                             ['waw_world/mc/shared', 'unchanged'])
            self.assertEqual(json.loads((project / 'BSP/constraints.json').read_text())['constraints'],
                             [{'material': 'waw_world/mc/shared'}, {'material': ''}])

    def test_default_use_hint_does_not_replace_authored_entity_hints(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'BSP').mkdir()
            entities = [{'classname': 'trigger_use', 'targetname': 'pickup'},
                        {'classname': 'trigger_use', 'targetname': 'authored', 'script_hint': 'source_hint'}]
            (root / 'BSP/entities.json').write_text(json.dumps({'entities': entities}))
            source = hintdefaults.source(root)
            self.assertIn('getentarray("pickup"', source)
            self.assertIn('PLATFORM_HOLD_TO_USE', source)
            self.assertNotIn('getentarray("authored"', source)

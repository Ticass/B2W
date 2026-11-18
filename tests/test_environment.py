from pathlib import Path
import tempfile
import unittest
import json
from unittest.mock import patch

from waw2bo2 import t6bridge


class EnvironmentTests(unittest.TestCase):
    def test_native_sky_bone_bounds_are_translated_without_losing_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw, target = root / 'bo2/raw', root / 'target'
            (raw / 'xmodel').mkdir(parents=True)
            (raw / 'materials/mc').mkdir(parents=True)
            bone = {'bone': 'sky', 'mins': {'z': -3, 'x': -1, 'y': -2},
                    'maxs': {'x': 1, 'y': 2, 'z': 3}, 'offset': {'x': 0, 'y': 0, 'z': 1},
                    'radiusSquared': 14, 'collmap': 0, 'hitloc': 'none'}
            model = {'lods': [], 'boneInfo': [bone], 'rootBoneName': 'sky'}
            (raw / f'xmodel/{t6bridge.SKYBOX_TEMPLATE}.json').write_text(json.dumps(model))
            (raw / f'materials/{t6bridge.SKYBOX_MATERIAL}.json').write_text(json.dumps({'textures': []}))
            report = t6bridge.StageReport('test')
            t6bridge.stage_skybox(report, 'test', target, root / 'bo2', None)
            self.assertEqual(report.errors, [])
            out = json.loads((target / 'xmodel/skybox_test.json').read_text())
            expected = dict(bone, mins=[-1, -2, -3], maxs=[1, 2, 3], offset=[0, 0, 1])
            self.assertEqual(out['boneInfo'], [expected])
            t6bridge.bridge_bone_bounds(out)
            self.assertEqual(out['boneInfo'], [expected])
            self.assertEqual(out['rootBoneName'], 'sky')

    def test_source_sky_preserves_all_layers_geometry_buffers_and_t6_model_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target, bo2 = root / 'source', root / 'target', root / 'bo2'
            def write(path, data):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(data))
            write(source / 'xmodel/source_sky.json', {'lods': [{'file': 'model_export/sky.gltf'}], 'flags': 0})
            write(source / 'model_export/sky.gltf', {'materials': [{'name': 'background'}, {'name': 'clouds'}],
                'buffers': [{'uri': 'sky.bin', 'byteLength': 4}]})
            (source / 'model_export/sky.bin').write_bytes(b'abcd')
            write(bo2 / f'raw/materials/{t6bridge.SKYBOX_MATERIAL}.json', {'stateBits': []})
            for name in ('background', 'clouds'):
                write(source / f'materials/{name}.json', {'textures': [{'image': name}], 'sortKey': 5})
            def bind(material, output, *args):
                output['techniqueSet'] = 'translated/' + material['textures'][0]['image']
                return {'active': [{'paired': True, 'slot': 2}], 'unsupported': []}
            report = t6bridge.StageReport('converted')
            with (patch.object(t6bridge.native, 'tool', return_value=None),
                  patch.object(t6bridge.shaderruntime, 'bind_material', side_effect=bind)):
                t6bridge.stage_source_skybox(report, 'converted', target, [source], 'source_sky', root, bo2)
            self.assertEqual(report.content['sky']['materials'], ['background', 'clouds'])
            self.assertEqual((target / 'model_export/sky.bin').read_bytes(), b'abcd')
            model = json.loads((target / 'xmodel/skybox_converted.json').read_text())
            self.assertEqual(model['_game'], 't6')
            self.assertIn('lightingOriginOffset', model)
            self.assertEqual(model['lods'][0]['file'], 'model_export/sky.gltf')

    def test_template_art_does_not_override_source_atmosphere_and_migrates_old_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / 'template/maps/mp/createart/zm_test_art.gsc'
            template.parent.mkdir(parents=True)
            template.write_text('main() { visionsetnaked("zm_test", 1); setdvar("r_lightGridContrast", 0); }')
            project = root / 'project'
            destination = project / 'maps/mp/createart/converted_art.gsc'
            destination.parent.mkdir(parents=True)
            destination.write_text(template.read_text().replace('zm_test', 'converted'))
            report = t6bridge.StageReport('converted')
            t6bridge.stage_template_scripts(report, 'converted', project, root / 'template', 'zm_test')
            self.assertNotIn('visionsetnaked', destination.read_text())
            self.assertNotIn('r_lightGrid', destination.read_text())
            destination.write_text('main() { custom_source_art(); }')
            t6bridge.stage_template_scripts(report, 'converted', project, root / 'template', 'zm_test')
            self.assertIn('custom_source_art', destination.read_text())


class HudMaterialTests(unittest.TestCase):
    def test_script_hud_shaders_are_collected_and_zoned(self):
        text = 'precacheShader( "hud_screwdriver" );\nself.hud SetShader("hud_icon_rake", 32, 32);\nx = "hud_no";'
        self.assertEqual(sorted(t6bridge.SCRIPT_SHADER_RE.findall(text)), ["hud_icon_rake", "hud_screwdriver"])
        with tempfile.TemporaryDirectory() as tmp:
            zone = t6bridge.write_zone(Path(tmp), "p", [], False, materials=["hud_screwdriver"])
            self.assertIn("material,hud_screwdriver\n", zone.read_text())

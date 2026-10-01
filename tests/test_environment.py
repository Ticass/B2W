from pathlib import Path
import tempfile
import unittest
import json
from unittest.mock import patch

from waw2bo2 import t6bridge


class EnvironmentTests(unittest.TestCase):
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
            with patch.object(t6bridge.shaderruntime, 'bind_material', side_effect=bind):
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

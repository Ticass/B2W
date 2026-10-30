import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import impactmaterials


class SourceImpactTests(unittest.TestCase):
    def test_native_mark_uses_source_art_shader_and_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project, dump, overlay, stock = [root/name for name in ('project', 'dump', 'overlay', 'stock')]
            source = project/'content_source/impact_materials'
            material = {'techniqueSet': 'waw/runtime_mark',
                        'stateBits': [{'srcBlendRgb': 'one', 'dstBlendRgb': 'invsrcalpha'}],
                        'textures': [{'name': 'colorMap', 'image': 'waw_world/impact_metal05_col'}]}
            for relative, data in {
                'stage.json': json.dumps({'materials': [{'name': 'wc/gfx_impact_metal05',
                    'file': 'materials/wc/gfx_impact_metal05.json', 'shader_runtime': {'active': [{'slot': 4}]}}]}),
                'materials/wc/gfx_impact_metal05.json': json.dumps(material),
                'techniquesets/waw/runtime_mark.json': '{}',
                'shader_bin/ps_waw/runtime_mark.cso': 'source program',
                'images/waw_world/impact_metal05_col.iwi': 'source pixels',
            }.items():
                p = source/relative
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(data)
            for name in ('wc/gfx_impact_metal05_blend', 'mc/gfx_impact_metal05_blend', 'wc/unrelated'):
                p = dump/'materials'/f'{name}.json'
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('{}')
            changed = impactmaterials.bind(project, dump, overlay, stock)
            self.assertEqual(changed, {'materials/wc/gfx_impact_metal05_blend.json': 'wc/gfx_impact_metal05'})
            result = json.loads((overlay/'materials/wc/gfx_impact_metal05_blend.json').read_text())
            self.assertEqual(result, material)
            self.assertEqual((overlay/'images/waw_world/impact_metal05_col.iwi').read_text(), 'source pixels')
            self.assertTrue((overlay/'shader_bin/ps_waw/runtime_mark.cso').exists())

    def test_nonconverted_project_has_no_impact_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(impactmaterials.bind(root/'project', root/'dump', root/'overlay', root/'stock'), {})

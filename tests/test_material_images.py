import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import iwi, shaderruntime, t6bridge, techsets, wavelet, weapons


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))


class MaterialImageTests(unittest.TestCase):
    def test_custom_unlit_decal_keeps_transparency_depth_and_draw_order(self):
        state = {'blendOpRgb': 'add', 'srcBlendRgb': 'srcalpha', 'dstBlendRgb': 'invsrcalpha',
                 'depthWrite': False, 'colorWriteRgb': True, 'polygonOffset': 'offset0'}
        source = {'techniqueSet': 'wc_unlit', 'stateBits': [state], 'stateBitsEntry': [-1]*4+[0],
                  'sortKey': 43, 'textures': []}
        self.assertEqual(techsets.material_techset(source), 'wc_unlit_blend')
        donor = {'techniqueSet': 'wpc_unlit_blend_12345678', 'cameraRegion': 'emissiveTrans',
                 'stateFlags': 21, 'stateBits': [dict(state, depthWrite=True, polygonOffset='offset1')],
                 'textures': []}
        out = techsets.build_material(source, donor, [])
        self.assertEqual(out['stateBits'][0], state)
        self.assertEqual(out['sortKey'], 43)
        self.assertEqual(out['cameraRegion'], 'emissiveTrans')
        self.assertEqual(out['stateFlags'], 21)
        source['stateBits'][0]['blendOpRgb'] = 'disabled'
        self.assertEqual(techsets.material_techset(source), 'wc_unlit')

    def test_emissive_route_uses_original_unlit_pair_and_preserves_explicit_pass(self):
        unlit = {'name': 'source_unlit'}
        original = {'techniques': [None] * 6}
        original['techniques'][4] = unlit
        for name in ('wc_unlit', 'mc_unlit', 'wc_unlit_blend', 'wc_unlit_distfalloff'):
            self.assertIs(shaderruntime.source_technique(original, name, 3), unlit)
        self.assertIsNone(shaderruntime.source_technique(original, 'wc_l_sm_r0c0', 3))
        emissive = {'name': 'source_emissive'}
        original['techniques'][5] = emissive
        self.assertIs(shaderruntime.source_technique(original, 'wc_unlit', 3), emissive)

    def test_distance_falloff_portal_keeps_multiplicative_blending(self):
        state = {'blendOpRgb': 'add', 'srcBlendRgb': 'destcolor', 'dstBlendRgb': 'srccolor',
                 'depthWrite': False, 'colorWriteRgb': True, 'polygonOffset': 'offset0'}
        source = {'techniqueSet': 'wc_unlit_distfalloff', 'stateBits': [state],
                  'stateBitsEntry': [-1]*4+[0], 'sortKey': 43}
        donor = {'stateBits': [dict(state, srcBlendRgb='srcalpha', dstBlendRgb='invsrcalpha')],
                 'cameraRegion': 'emissiveTrans', 'sortKey': 40}
        out = techsets.build_material(source, donor, [])
        self.assertEqual(out['stateBits'][0], state)
        self.assertEqual(out['sortKey'], 43)
        self.assertEqual(out['cameraRegion'], 'emissiveTrans')

    def test_weapon_visuals_resolve_original_pixels_before_namespacing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, native, project = root / 'source', root / 'native', root / 'project'
            texture = {'name': 'colorMap', 'image': 'original', 'semantic': 'colorMap'}
            write_json(source / 'materials/custom.json', {'techniqueSet': '2d', 'textures': [texture]})
            write_json(native / 'materials/hud.json', {'techniqueSet': 'trivial_12345678', 'textures': [texture]})
            write_json(native / 'techniquesets/trivial_12345678.json', {'techniques': []})
            write_json(project / 'weapons.stage.json', {'models': {'materials': ['custom']}, 'dependencies': {}})
            pixels = bytes([20, 40, 80, 255]) * 4
            dds = wavelet.WaveletImage(2, 2, 6, 0, [pixels]).dds()
            (source / 'images').mkdir()
            (source / 'images/original.dds').write_bytes(dds)
            with patch.object(t6bridge.shaderruntime, 'bind_material', return_value={'active': []}):
                result = weapons.stage_visuals([source], project, native / 'materials', native)
            self.assertEqual(result['failed_materials'], {})
            self.assertEqual(result['material_images']['waw_material/custom'], ['waw_image/original'])
            self.assertEqual((project / 'images/waw_image/original.iwi').read_bytes(), iwi.dds_to_iwi(dds))

    def test_absent_material_equivalent_preserves_native_pixels_and_source_wins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, native, project = root / 'source', root / 'native', root / 'project'
            material = {'techniqueSet': 'trivial_12345678', 'textures': [
                {'name': 'colorMap', 'image': 'native_color', 'semantic': 'colorMap'}]}
            path = native / 'materials/equivalent.json'
            write_json(path, material)
            write_json(native / 'techniquesets/trivial_12345678.json', {'techniques': []})
            (native / 'images').mkdir()
            blob = iwi.solid_rgba_iwi(2, 2, (10, 20, 30, 255))
            (native / 'images/native_color.iwi').write_bytes(blob)
            report = t6bridge.StageReport('any_map')
            t6bridge.stage_materials(report, {'missing'}, [source], native / 'materials', project, native,
                                    native_fallbacks={'missing': path})
            name = 'waw_world/bo2_fallback/native_color'
            self.assertEqual((project / f'images/{name}.iwi').read_bytes(), blob)
            written = t6bridge.stage_images(report, [], project, iwd_dirs=[])
            self.assertIn(name, written)
            self.assertEqual((project / f'images/{name}.iwi').read_bytes(), blob)
            self.assertTrue(any('BO2_FALLBACK material missing' in w for w in report.warnings))
            write_json(source / 'materials/missing.json', {'techniqueSet': '2d', 'textures': [
                {'name': 'colorMap', 'image': 'source_color', 'semantic': 'colorMap'}]})
            report = t6bridge.StageReport('another_map')
            with patch.object(t6bridge.shaderruntime, 'bind_material', return_value={'active': []}):
                t6bridge.stage_materials(report, {'missing'}, [source], native / 'materials', project, native,
                                        native_fallbacks={'missing': path})
            actual = json.loads((project / 'materials/missing.json').read_text())
            self.assertEqual(actual['textures'][0]['image'], 'waw_world/source_color')
            self.assertFalse(any('BO2_FALLBACK' in w for w in report.warnings))

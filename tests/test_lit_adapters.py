import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import lightmaps, shaders, shaderruntime as runtime

LIT_VS = '''//   fogConsts            c21      1
    vs_3_0
    def c9, 0.00787401572, 0.00392156886, -1, 0.752941191
    dcl_position v0
    dcl_normal v3
    dcl_position o0
    dcl_texcoord1 o3
    mov o0, v0
    mad r1, v3, c9.xxxy, c9.zzzw
    mul r4.xyz, r1.w, r1
    mov r0.w, c21.x
    mad r0.w, r0.w, v0.z, c9.z
    exp r0.w, r0.w
    dp3 o3.x, r4, r4
    mov o3.y, r4.y
    mov o3.z, r4.z
    min o3.w, r0.w, -c9.z
'''


class TaintTests(unittest.TestCase):
    def test_fog_found_through_scalar_ops_and_dp3_reads_xyz_only(self):
        # r0.w carries fog; dp3 of a register whose .w is tainted must not taint
        source = LIT_VS.replace('dp3 o3.x, r4, r4', 'mov r4.w, r0.w\n    dp3 o3.x, r4, r4')
        self.assertEqual(runtime.tainted_outputs(source, {'c21'}), {('o3', 'w')})

    def test_overwrite_clears_taint(self):
        source = 'vs_3_0\nmov r0, c21\nmov r0.xy, c1\nmov o0, r0\n'
        self.assertEqual(runtime.tainted_outputs(source, {'c21'}), {('o0', 'z'), ('o0', 'w')})


class HashTests(unittest.TestCase):
    def test_waw_material_hash_matches_dumped_arguments(self):
        self.assertEqual(runtime.waw_hash('envMapParms'), 1033475292)
        self.assertEqual(runtime.waw_hash('colorMap'), 2695565377)
        self.assertEqual(runtime.material_literal({'constants': [{'name': 'envMapParms', 'literal': [1, 2, 3, 4]}]},
                                                  1033475292), [1.0, 2.0, 3.0, 4.0])


class TranslatorAdapterTests(unittest.TestCase):
    def test_packed_vector_adapter_reencodes_unit_scale(self):
        contract = {'inputs': {'v0': {'semantic': 'POSITION0', 'width': 4},
                               'v3': {'semantic': 'NORMAL0', 'width': 3, 'adapter': 'waw_ubyte4_vector'}},
                    'outputs': {'o0': {'semantic': 'SV_Position', 'width': 4}, 'o3': {'semantic': 'TEXCOORD1', 'width': 4}},
                    'constants': {'c21': {'buffer': 0, 'index': 27}}, 'samplers': {}}
        hlsl, _ = shaders.translate(LIT_VS, contract)
        self.assertIn('(normalize(wrap_v3) + 1.0) * 127.0, 63.0', hlsl)
        # w = 63 decodes to a scale of exactly 1 with WaW's literal
        self.assertAlmostEqual(63 / 255 + 0.752941191, 1.0, places=6)
        shaders.compile_hlsl(hlsl, 'vs_5_0')

    def test_paired_pixel_signature_mirrors_vertex_outputs(self):
        ps = 'ps_3_0\ndcl_texcoord1 v0\nmov oC0, v0\n'
        linked = [{'semantic': 'SV_Position', 'width': 4}, {'semantic': 'COLOR0', 'width': 4},
                  {'semantic': 'TEXCOORD1', 'width': 4}]
        contract = {'inputs': {'v0': {'semantic': 'TEXCOORD1', 'width': 4}},
                    'outputs': {'oC0': {'semantic': 'SV_Target0', 'width': 4}},
                    'constants': {}, 'samplers': {}, 'input_signature': linked}
        hlsl, _ = shaders.translate(ps, contract)
        code = shaders.compile_hlsl(hlsl, 'ps_5_0')
        signature = runtime._compiled_signature(code, 'Input')
        self.assertEqual(signature[('TEXCOORD', 1)][0], 2)
        self.assertEqual(signature[('SV_POSITION', 0)][0], 0)

    def test_aliased_lightmap_samplers_share_one_slot_with_remapped_v(self):
        ps = '''ps_3_0
dcl_texcoord v0
dcl_2d s2
dcl_2d s3
texld r0, v0.zwzw, s3
texld r1, v0.zwzw, s2
add oC0, r0, r1
'''
        contract = {'inputs': {'v0': {'semantic': 'TEXCOORD0', 'width': 4}},
                    'outputs': {'oC0': {'semantic': 'SV_Target0', 'width': 4}}, 'constants': {},
                    'samplers': {'s3': {'texture': 13, 'sampler': 13, 'v_scale_offset': list(lightmaps.WAW_PAGE_UV['lightmapSamplerSecondary'])},
                                 's2': {'texture': 13, 'sampler': 13, 'v_scale_offset': list(lightmaps.WAW_PAGE_UV['lightmapSamplerPrimary'])}}}
        hlsl, _ = shaders.translate(ps, contract)
        self.assertEqual(hlsl.count('register(t13)'), 1)
        self.assertIn('0.6666666666666666', hlsl)
        shaders.compile_hlsl(hlsl, 'ps_5_0')

    def test_point_load_reads_texels_without_sampler_state(self):
        ps = 'ps_3_0\ndcl_texcoord v0\ndcl_2d s5\ntexldl r0, v0, s5\nmov oC0, r0\n'
        contract = {'inputs': {'v0': {'semantic': 'TEXCOORD0', 'width': 4}},
                    'outputs': {'oC0': {'semantic': 'SV_Target0', 'width': 4}}, 'constants': {},
                    'samplers': {'s5': {'texture': 9, 'sampler': 9, 'point_load': True}}}
        hlsl, _ = shaders.translate(ps, contract)
        self.assertNotIn('SamplerState samp_s5', hlsl)
        self.assertIn('tex_s5.Load', hlsl)
        shaders.compile_hlsl(hlsl, 'ps_5_0')


def _dds(width, height, rgba=None, lum=None):
    header = bytearray(128)
    header[:4] = b'DDS '
    struct.pack_into('<7I', header, 4, 124, 0x100F, height, width, 0, 1, 1)
    if lum is not None:
        struct.pack_into('<2I4s5I', header, 76, 32, 0x20000, b'\0\0\0\0', 8, 0xFF, 0, 0, 0)
        return bytes(header) + lum
    struct.pack_into('<2I4s5I', header, 76, 32, 0x41, b'\0\0\0\0', 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000)
    return bytes(header) + rgba


class LightmapPageTests(unittest.TestCase):
    def test_waw_page_stacks_halves_and_primary_and_t6_page_squares_flat_lighting(self):
        w = 2
        top = bytes([128, 64, 255, 255]) * (w * w)      # colour A, dir alpha
        bottom = bytes([0, 0, 0, 0]) * (w * w)          # colour B = 0
        with tempfile.TemporaryDirectory() as tmp:
            sec, pri = Path(tmp) / 's.dds', Path(tmp) / 'p.dds'
            sec.write_bytes(_dds(w, 2 * w, rgba=top + bottom))
            pri.write_bytes(_dds(2 * w, 2 * w, lum=bytes([200, 100] * (2 * w * w))))
            width, height, waw, t6 = lightmaps.build_page(sec, pri)
        self.assertEqual((width, height), (w, w))
        n = w * w * 4
        self.assertEqual(waw[:n], top)
        self.assertEqual(waw[n:2 * n], bottom)
        self.assertEqual(waw[2 * n], 150)  # 2x2 box of 200/100 columns
        # flat lighting = A (B = 0); T6 stores A^2 as rgb / a
        r, g, b, a = t6[:4]
        decoded = [c / 255 / (a / 255) for c in (r, g, b)]
        for got, want in zip(decoded, (128 / 255, 64 / 255, 1.0)):
            self.assertAlmostEqual(got, want * want, delta=0.01)
        self.assertEqual(t6[2 * n + 3], 150)  # sun visibility in the third page alpha


    def test_rectangular_page_preserves_rows_and_filters_primary_in_both_axes(self):
        w, h = 2, 4
        top = b''.join(bytes([20 + y, 40, 80, 255]) * w for y in range(h))
        bottom = b''.join(bytes([0, 0, 0, y]) * w for y in range(h))
        # Every 2x2 primary block averages to 20*y + 5, retaining row order.
        primary = b''.join(bytes([20 * (y // 2), 20 * (y // 2) + 10]) * w
                           for y in range(2 * h))
        with tempfile.TemporaryDirectory() as tmp:
            sec, pri = Path(tmp) / 's.dds', Path(tmp) / 'p.dds'
            sec.write_bytes(_dds(w, 2 * h, rgba=top + bottom))
            pri.write_bytes(_dds(2 * w, 2 * h, lum=primary))
            width, height, waw, t6 = lightmaps.build_page(sec, pri)
        self.assertEqual((width, height), (w, h))
        n = w * h * 4
        self.assertEqual(len(waw), 3 * n)
        self.assertEqual(len(t6), 3 * n)
        self.assertEqual(waw[:2 * n], top + bottom)
        visibility = b''.join(bytes([20 * y + 5]) * w for y in range(h))
        self.assertEqual(waw[2 * n::4], visibility)
        self.assertEqual(t6[2 * n + 3::4], visibility)
        for y in range(h):
            pixel = t6[y * w * 4:y * w * 4 + 4]
            self.assertAlmostEqual(pixel[0] / pixel[3], ((20 + y) / 255) ** 2, delta=0.002)

    def test_reported_512_by_2048_secondary_stages_512_by_3072_images(self):
        w, h = 512, 1024
        n = w * h * 4
        top = bytes([128, 64, 255, 255]) * (w * h)
        bottom = bytes([0, 0, 0, 0]) * (w * h)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / '_lightmap0_secondary.dds').write_bytes(_dds(w, 2 * h, rgba=top + bottom))
            world = SimpleNamespace(surfaces=[SimpleNamespace(material='wall', lightmap_index=0)])
            report = lightmaps.stage(world, [root], root / 'project', {'wall'})
            self.assertEqual(report['errors'], [])
            self.assertEqual((report['pages'][0]['width'], report['pages'][0]['height']), (w, h))
            plan = json.loads((root / 'project/BSP/lightmaps.json').read_text())
            for encoding in ('waw', 't6'):
                blob = (root / 'project/images' / (plan['pages'][0][encoding] + '.iwi')).read_bytes()
                self.assertEqual(struct.unpack_from('<HH', blob, 6), (w, 3 * h))
                self.assertEqual(struct.unpack_from('<I', blob, 32)[0], len(blob))
                self.assertEqual(len(blob), 64 + 3 * n)
                if encoding == 'waw':
                    self.assertEqual(blob[64:64 + 2 * n], top + bottom)
                self.assertEqual(blob[64 + 2 * n + 3::4], bytes([255]) * (w * h))

    def test_odd_secondary_height_and_incompatible_primary_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            sec, pri = Path(tmp) / 's.dds', Path(tmp) / 'p.dds'
            sec.write_bytes(_dds(2, 7, rgba=bytes(2 * 7 * 4)))
            with self.assertRaisesRegex(lightmaps.LightmapError, 'two equal stacked'):
                lightmaps.build_page(sec, None)
            sec.write_bytes(_dds(2, 8, rgba=bytes(2 * 8 * 4)))
            pri.write_bytes(_dds(4, 6, lum=bytes(4 * 6)))
            with self.assertRaisesRegex(lightmaps.LightmapError, 'integer multiple'):
                lightmaps.build_page(sec, pri)


if __name__ == '__main__':
    unittest.main()


class LocalLightTests(unittest.TestCase):
    # T6 PerSceneConsts rows of the light uniforms (stock world lit programs)
    FIELDS = {'hdrControl0': (0, 320, 16), 'lightPosition': (0, 1232, 16), 'lightDiffuse': (0, 1248, 16),
              'lightSpotDir': (0, 1264, 16), 'lightSpotFactors': (0, 1280, 16), 'lightFallOffA': (0, 1312, 16),
              'lightFallOffB': (0, 1328, 16)}

    def test_waw_light_uniforms_come_from_t6_light_rows(self):
        position = runtime.light_constant('lightPosition', self.FIELDS, 20, None)
        self.assertEqual(position['expr'], 'float4(t6_cb0[77].xyz, -t6_cb0[82].w)')
        factors = runtime.light_constant('lightSpotFactors', self.FIELDS, 20, None)
        self.assertEqual(factors['expr'], 'float4(t6_cb0[82].x, t6_cb0[83].x, t6_cb0[82].y, t6_cb0[80].w)')
        specular = runtime.light_constant('lightSpecular', self.FIELDS, 20, None)
        self.assertIn('t6_cb0[78].xyz * (4.0 * t6_cb0[20].x)', specular['expr'])
        self.assertEqual(runtime.light_constant('lightSpotDir', self.FIELDS, 20, None)['expr'],
                         'float4(t6_cb0[79].xyz, 0.0)')
        placement = runtime.light_constant('lightFalloffPlacement', self.FIELDS, 20, (0.5, 0.0, 0.25, 0.125))
        self.assertEqual(placement, {'expr': 'float4(0.5, 0.0, 0.25, 0.125)', 'uses': []})
        with self.assertRaises(shaders.ShaderError):
            runtime.light_constant('lightFalloffPlacement', self.FIELDS, 20, None)
        with self.assertRaises(shaders.ShaderError):
            runtime.light_constant('lightDiffuse', self.FIELDS, None, None)

    def test_light_rows_read_by_a_program_become_pass_arguments(self):
        # T6 copies per-light code constants into a pass only through type-5 arguments
        native = {'perPrimArgCount': 1, 'perObjArgCount': 1, 'stableArgCount': 1, 'args': [
            {'type': 3, 'location': 0, 'buffer': 3, 'size': 64, 'u': {'value': 1}},
            {'type': 3, 'location': 576, 'buffer': 0, 'size': 64, 'u': {'value': 2}},
            {'type': 5, 'location': 1232, 'buffer': 0, 'size': 16, 'u': {'value': 0x01000000}}]}
        runtime.light_constant('lightPosition', self.FIELDS, 20, None, native)
        runtime.light_constant('lightSpotFactors', self.FIELDS, 20, None, native)
        rows = [(a['location'], a['u']['value']) for a in native['args'] if a['type'] == 5]
        self.assertEqual(rows, [(1232, 0x01000000), (1280, 0x01000003), (1312, 0x01000005), (1328, 0x01000006)])
        self.assertEqual(native['stableArgCount'], 4)

    def test_falloff_placement_uses_the_shared_light_def(self):
        import json
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'lightdef').mkdir()
            (root / 'images').mkdir()
            (root / 'lightdef/point.json').write_text(json.dumps({'attenuation': ',falloff', 'lmapLookupStart': 1}))
            (root / 'images/falloff.dds').write_bytes(_dds(16, 1, lum=bytes(16)))
            (root / 'images/_lightmap0_secondary.dds').write_bytes(_dds(4, 8, rgba=bytes(4 * 8 * 4)))
            lights = root / 'lights.json'
            table = [{'type': 1, 'defName': ''}, {'type': 2, 'defName': 'point'}, {'type': 3, 'defName': 'point'}]
            lights.write_text(json.dumps({'lights': table}))
            # WaW sub_7425D0: (width / 512, 0, lmapLookupStart / 512, 0); v = row 0 centre
            self.assertEqual(runtime.light_falloff_placement(lights, [root]), (16 / 512, 0.0, 1 / 512, 0.5 / 8))
            table.append({'type': 2, 'defName': 'other'})
            lights.write_text(json.dumps({'lights': table}))
            self.assertIsNone(runtime.light_falloff_placement(lights, [root]))


class LitSlotTests(unittest.TestCase):
    def test_dynamic_light_slots_extend_reachable_lit_slots(self):
        for slot, base in runtime.DLIGHT_BASE_SLOTS.items():
            self.assertIn(slot, runtime.REACHABLE_LIT_SLOTS)
            self.assertIn(base, runtime.REACHABLE_LIT_SLOTS)
            self.assertEqual(slot in runtime.SHADOWED_LIT_SLOTS, base in runtime.SHADOWED_LIT_SLOTS)
        # square/round spot techniques are unreachable for staged plain spots
        self.assertFalse(set(range(9, 13)) & set(runtime.REACHABLE_LIT_SLOTS))
        self.assertFalse(set(range(20, 24)) & set(runtime.REACHABLE_LIT_SLOTS))

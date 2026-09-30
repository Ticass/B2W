import struct
import tempfile
import unittest
from pathlib import Path

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
            width, waw, t6 = lightmaps.build_page(sec, pri)
        self.assertEqual(width, w)
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


if __name__ == '__main__':
    unittest.main()

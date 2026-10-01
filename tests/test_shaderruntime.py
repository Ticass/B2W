import os
import unittest
from unittest.mock import patch
from pathlib import Path

from waw2bo2 import shaders, shaderruntime as runtime


NATIVE = '''// cbuffer PerScene
// {
// float4 fogColor; // Offset: 416 Size: 16
// }
// PerScene cbuffer NA NA cb0 1
// Output signature:
// Name Index Mask Register SysValue Format Used
// COLOR 0 xyzw 0 NONE float xyzw
// TEXCOORD 0 xyz 1 NONE float xyz
vs_5_0
'''
TARGET = '''// Output signature:
// SV_Target 0 xyzw 0 TARGET float xyzw
ps_5_0
'''
SOURCE = '''// fogColor c7 1
ps_3_0
dcl_color_pp v0
dcl_texcoord_centroid v1.xy
dcl_2d s0
texld r0, v1, s0
mul r0, r0, v0
add oC0, r0, c7
'''


class RuntimeShaderTests(unittest.TestCase):
    def test_missing_donor_constant_is_removed_without_changing_frequency_groups(self):
        args = [{'type': 3, 'buffer': 3, 'location': 0, 'size': 64, 'u': {'value': 123}},
                {'type': 0, 'buffer': 2, 'location': 16, 'size': 16, 'u': {'value': 456}},
                {'type': 6, 'buffer': 2, 'location': 32, 'size': 16, 'u': {'value': 789}}]
        native = {'args': args, 'perPrimArgCount': 1, 'perObjArgCount': 0, 'stableArgCount': 2}
        runtime.prune_missing_material_constants(native, {'constants': [{'nameHash': 789}]},
                                                ({'constants': {}},))
        self.assertEqual(native['args'], [args[0], args[2]])
        self.assertEqual(native['perPrimArgCount'], 1)
        self.assertEqual(native['stableArgCount'], 1)
        with self.assertRaisesRegex(shaders.ShaderError, 'absent donor'):
            runtime.prune_missing_material_constants(native, {'constants': []},
                ({'constants': {'c0': {'buffer': 2, 'index': 2}}},))

    def test_named_constant_with_underscore_matches_t6_argument_hash(self):
        # Stock zm_nuked: Flicker_Min binds as 0x2ddf50e9 (T6 ORs 0x20 per byte).
        self.assertEqual(runtime.t6_hash('Flicker_Min'), 0x2ddf50e9)
        args = [{'type': 0, 'buffer': 2, 'location': 16, 'size': 16, 'u': {'value': 0x2ddf50e9}}]
        native = {'args': list(args), 'perPrimArgCount': 0, 'perObjArgCount': 0, 'stableArgCount': 1}
        runtime.prune_missing_material_constants(native, {'constants': [{'name': 'Flicker_Min'}]},
                                                ({'constants': {}},))
        self.assertEqual(native['args'], args)

    def test_source_sky_keeps_vertex_color_uv_and_cloud_time(self):
        native = '''// cbuffer PerScene
// {
// float4x4 viewProjectionMatrix; // Offset: 576 Size: 64
// }
// cbuffer PerObject
// {
// float4x4 worldMatrix; // Offset: 0 Size: 64
// }
// PerScene cbuffer NA NA cb0 1
// PerObject cbuffer NA NA cb3 1
// Input signature:
// POSITION 0 xyz 0 NONE float xyz
vs_5_0
'''
        source = '''// gameTime c22 1
vs_3_0
def c4, 0, 7700, 0, 0
dcl_position v0
dcl_color v1
dcl_texcoord v2.xy
dcl_position o0
dcl_color o1
dcl_texcoord o2
mov o0, v0
mov o1, v1
add o2.xy, v2, c22
nrm r0.xyz, v0
mul r0.xyz, r0, c4.y
'''
        target_pass = {'args': []}
        with patch.object(runtime, '_native_program', return_value=native):
            contract = runtime.vertex_contract(source,
                {'args': [{'type': 3, 'dest': 22, 'rowCount': 1}]}, target_pass,
                Path('.'), paired=True, source_material={'techniqueSet': 'mc_sky_noncubemap'})
        self.assertEqual(contract['inputs']['v1']['semantic'], 'COLOR0')
        self.assertEqual(contract['inputs']['v2']['semantic'], 'TEXCOORD0')
        self.assertEqual(contract['constants']['c22'], {'buffer': 2, 'index': 15})
        self.assertEqual(target_pass['args'][0]['u']['value'] & 0xffff, 25)
        self.assertEqual(target_pass['args'][0]['u']['value'] >> 24, 1)
        self.assertEqual(target_pass['stableArgCount'], 1)
        self.assertEqual(contract['sky_fog_distance'], 7700)
        hlsl, _ = shaders.translate(source, contract)
        if os.name == 'nt':
            self.assertEqual(shaders.compile_hlsl(hlsl, 'vs_5_0')[:4], b'DXBC')

    def test_original_hash_and_native_byte_offsets_bind_resources(self):
        source_pass = {'args': [{'type': 5, 'dest': 7, 'rowCount': 1},
                                {'type': 2, 'dest': 0, 'value': 1234}]}
        target_pass = {'args': [{'type': 2, 'location': 5 | (3 << 8), 'u': {'value': 1234}}]}
        with patch.object(runtime, '_native_program', side_effect=lambda _, stage, __: NATIVE if stage == 'vs' else TARGET):
            contract = runtime.pixel_contract(SOURCE, source_pass, target_pass, Path('.'))
            self.assertEqual(contract['constants']['c7'], {'buffer': 0, 'index': 26})
            self.assertEqual(contract['samplers']['s0'], {'texture': 5, 'sampler': 3})
            hlsl, info = shaders.translate(SOURCE, contract)
            self.assertEqual(info['inputs']['v0'][0], 'COLOR0')
            self.assertIn('centroid float2 v1 : TEXCOORD0', hlsl)
            if os.name == 'nt':
                self.assertEqual(shaders.compile_hlsl(hlsl, 'ps_5_0')[:4], b'DXBC')
            target_pass['args'][0]['u']['value'] = 4321
            with self.assertRaisesRegex(shaders.ShaderError, 'texture binding absent'):
                runtime.pixel_contract(SOURCE, source_pass, target_pass, Path('.'))

    def test_literals_remove_cpu_binding_requirement(self):
        source = runtime.embed_literals('ps_3_0\nmov oC0, c4',
            {'args': [{'type': 7, 'dest': 4, 'literal': [1, 2, 3, 4]}]}, 'ps')
        _, info = shaders.translate(source)
        self.assertEqual(info['constants'], [])

    def test_vertex_packing_adapters_compile_and_require_explicit_opt_in(self):
        assembly = '''vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_position o0
dcl_texcoord o1
mov o0, v0
mov o1, v1
'''
        contract = {'inputs': {'v0': {'semantic': 'POSITION0', 'width': 3, 'adapter': 'waw_position'},
                               'v1': {'semantic': 'TEXCOORD0', 'width': 2, 'adapter': 'waw_half_uv'}},
                    'outputs': {'o0': {'semantic': 'SV_Position', 'width': 4},
                                'o1': {'semantic': 'TEXCOORD0', 'width': 4}}}
        hlsl, _ = shaders.translate(assembly, contract)
        self.assertIn('float4(input.v0, 1)', hlsl)
        self.assertIn('half_v1.y & 255', hlsl)
        if os.name == 'nt':
            self.assertEqual(shaders.compile_hlsl(hlsl, 'vs_5_0')[:4], b'DXBC')
        del contract['inputs']['v1']['adapter']
        with self.assertRaisesRegex(shaders.ShaderError, 'truncates'):
            shaders.translate(assembly, contract)

    def test_soft_particle_depth_adapter_preserves_camera_distance(self):
        source = """// floatZSampler s0 1
ps_3_0
dcl_texcoord v0.xy
dcl_2d s0
texld oC0, v0, s0
"""
        native = """// cbuffer PerScene
// {
// float4 zNear; // Offset: 928 Size: 16
// }
// PerScene cbuffer NA NA cb0 1
// floatZSampler texture float4 2d t1 1
// Output signature:
// SV_Target 0 xyzw 0 TARGET float xyzw
ps_5_0
dcl_sampler s1, mode_default
div r0.x, cb0[58].x, r0.x
"""
        with patch.object(runtime, '_native_program', side_effect=lambda _, stage, __: NATIVE if stage == 'vs' else native):
            contract = runtime.paired_pixel_contract(source, {'args': [{'type': 4, 'dest': 0}]},
                {'args': []}, Path('.'), [{'semantic': 'TEXCOORD0', 'width': 2}])
        hlsl, _ = shaders.translate(source, contract)
        self.assertIn('t6_cb0[58].x / max(abs(', hlsl)
        for distance in (1.0, 50.0, 10000.0):
            near, reciprocal = 0.1, 0.1 / distance
            self.assertAlmostEqual(near / reciprocal, distance)
        if os.name == 'nt':
            self.assertEqual(shaders.compile_hlsl(hlsl, 'ps_5_0')[:4], b'DXBC')

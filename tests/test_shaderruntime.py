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
    def test_distance_falloff_recovers_object_space_camera_and_xy_stream(self):
        native = '''// cbuffer PerScene
// {
// float4x4 inverseViewMatrix; // Offset: 640 Size: 64
// }
// cbuffer PerObject
// {
// float4x4 worldMatrix; // Offset: 0 Size: 64
// }
// PerScene cbuffer NA NA cb0 1
// PerObject cbuffer NA NA cb3 1
// Input signature:
// POSITION 0 xyzw 0 NONE float xyzw
// TEXCOORD 0 xy 1 NONE float xy
vs_5_0
'''
        source = '''// inverseWorldViewMatrix c28 2
vs_3_0
dcl_position v0
dcl_texcoord v1
dcl_position o0
dcl_texcoord o1.xy
mov r0.x, c28.w
mov r0.y, c29.w
add r0.xy, r0, -v1
mov o0, v0
mov o1.xy, v1
'''
        args = {'args': [{'type': 3, 'dest': 28, 'rowCount': 2, 'firstRow': 0}]}
        with patch.object(runtime, '_native_program', return_value=native):
            contract = runtime.vertex_contract(source, args, {'args': []}, Path('.'), paired=True)
            self.assertEqual(contract['inputs']['v1']['adapter'], 'waw_uv')
            self.assertIn('cross(', contract['constants']['c28']['expr'])
            self.assertIn('t6_cb0[40].w - t6_cb3[0].w', contract['constants']['c28']['expr'])
            self.assertIn([3, 2], contract['constants']['c29']['uses'])
            hlsl, _ = shaders.translate(source, contract)
            if os.name == 'nt':
                self.assertEqual(shaders.compile_hlsl(hlsl, 'vs_5_0')[:4], b'DXBC')
            with self.assertRaisesRegex(shaders.ShaderError, 'full matrix adapter'):
                runtime.vertex_contract(source.replace('c28.w', 'c28.x'), args, {'args': []}, Path('.'), paired=True)

    def test_direct_uv_copy_does_not_require_a_lightmap_or_half_decoder(self):
        source = 'vs_3_0\ndcl_position v1\ndcl_texcoord v0\ndcl_position o1\ndcl_texcoord o0.xy\nmov o1, v1\nmov o0.xy, v0\n'
        native = '// Input signature:\n// POSITION 0 xyzw 1 NONE float xyzw\n// TEXCOORD 0 xy 0 NONE float xy\nvs_5_0\n'
        with patch.object(runtime, '_native_program', return_value=native):
            contract = runtime.vertex_contract(source, {'args': []}, {'args': []}, Path('.'), paired=True)
            self.assertEqual(contract['inputs']['v0']['adapter'], 'waw_uv')
            hlsl, _ = shaders.translate(source, contract)
            self.assertIn('float4(input.v0, 0, 0)', hlsl)
            if os.name == 'nt':
                self.assertEqual(shaders.compile_hlsl(hlsl, 'vs_5_0')[:4], b'DXBC')
            with self.assertRaises(shaders.ShaderError):
                runtime.vertex_contract(source.replace('o0.xy, v0', 'o0.xy, v0.zw'),
                                        {'args': []}, {'args': []}, Path('.'), paired=True)

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

    def test_unread_donor_constants_are_removed_from_paired_passes(self):
        args = [{'type': 6, 'buffer': 2, 'location': 16, 'size': 16, 'u': {'value': 0xe27483cf}},
                {'type': 6, 'buffer': 2, 'location': 32, 'size': 16, 'u': {'value': 789}}]
        native = {'args': list(args), 'perPrimArgCount': 0, 'perObjArgCount': 0, 'stableArgCount': 2}
        material = {'constants': [{'nameHash': 0xe27483cf}, {'nameHash': 789}]}
        runtime.prune_missing_material_constants(native, material,
                                                ({'constants': {'c0': {'buffer': 2, 'index': 2}}},), unread=True)
        self.assertEqual(native['args'], [args[1]])
        self.assertEqual(native['stableArgCount'], 1)

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


    def test_per_surface_textures_set_the_pass_sampler_flags(self):
        # T6 binds the surface lightmap (t13) and probe (t15) only for passes
        # whose customSamplerFlags ask for them (bit 1 lightmap, bit 0 probe).
        source = """// lightmapSamplerSecondary s0 1
// reflectionProbeSampler s1 1
ps_3_0
dcl_texcoord v0.xy
dcl_texcoord1 v1.xyz
dcl_2d s0
dcl_cube s1
texld r0, v0, s0
texld r1, v1, s1
add oC0, r0, r1
"""
        native = """// cbuffer PerScene
// {
// float4 hdrControl0; // Offset: 320 Size: 16
// }
// PerScene cbuffer NA NA cb0 1
// Output signature:
// SV_Target 0 xyzw 0 TARGET float xyzw
ps_5_0
sqrt o0.xyz, r0.xyzx
"""
        native_pass = {'args': [], 'customSamplerFlags': 0}
        with patch.object(runtime, '_native_program', side_effect=lambda _, stage, __: NATIVE if stage == 'vs' else native):
            contract = runtime.paired_pixel_contract(source, {'args': [{'type': 4, 'dest': 0}, {'type': 4, 'dest': 1}]},
                native_pass, Path('.'), [{'semantic': 'TEXCOORD0', 'width': 2}, {'semantic': 'TEXCOORD1', 'width': 3}])
        self.assertEqual(contract['samplers']['s0']['texture'], 13)
        self.assertEqual(contract['samplers']['s1']['texture'], 15)
        self.assertEqual(native_pass['customSamplerFlags'], 3)

    def test_waw_alpha_test_becomes_a_discard(self):
        material = {'techniqueSet': 'wc_l_sm_r0c0', 'stateBitsEntry': [-1] * 8 + [0],
                    'stateBits': [{'alphaTest': 'ge128'}]}
        self.assertEqual(runtime.alpha_test_lines(material, 'wc_l_sm_r0c0', 4, 'oC0'), ['if (oC0.w < 0.5) discard;'])
        material['stateBits'][0]['alphaTest'] = 'gt0'
        self.assertEqual(runtime.alpha_test_lines(material, 'wc_l_sm_r0c0', 4, 'oC0'), ['if (oC0.w <= 0.0) discard;'])
        self.assertTrue(runtime.alpha_tested(material))
        material['stateBits'][0]['alphaTest'] = 'disabled'
        self.assertEqual(runtime.alpha_test_lines(material, 'wc_l_sm_r0c0', 4, 'oC0'), [])
        self.assertFalse(runtime.alpha_tested(material))

class DisplayScaleTests(unittest.TestCase):
    def test_waw_scene_passes_write_half_their_display_colour(self):
        opaque = {'blendOpRgb': 'disabled'}
        blend = {'blendOpRgb': 'add', 'srcBlendRgb': 'srcalpha', 'dstBlendRgb': 'invsrcalpha'}
        multiply = {'blendOpRgb': 'add', 'srcBlendRgb': 'destcolor', 'dstBlendRgb': 'zero'}
        double_multiply = {'blendOpRgb': 'add', 'srcBlendRgb': 'destcolor', 'dstBlendRgb': 'srccolor'}
        self.assertTrue(runtime.scales_output(opaque))
        self.assertTrue(runtime.scales_output(blend))
        self.assertFalse(runtime.scales_output(multiply))
        self.assertFalse(runtime.scales_output(double_multiply))
        material = {'stateBitsEntry': [-1] * 4 + [0], 'stateBits': [blend]}
        self.assertEqual(runtime._output_scale(material, 'wc_unlit', 2), {'output_rgb_scale': 0.5})
        self.assertEqual(runtime._output_scale(material, '2d', 2), {})
        self.assertEqual(runtime._output_scale(material, 'wc_unlit', 0), {})

    def test_output_scale_keeps_alpha(self):
        hlsl, _ = shaders.translate('ps_3_0\ndcl_texcoord v0\nmov oC0, v0\n',
                                    {'inputs': {'v0': {'semantic': 'TEXCOORD0', 'width': 4}},
                                     'outputs': {'oC0': {'semantic': 'SV_Target0', 'width': 4}},
                                     'output_rgb_scale': 0.5})
        self.assertIn('output.oC0 = float4(oC0.xyz * 0.5, oC0.w);', hlsl)

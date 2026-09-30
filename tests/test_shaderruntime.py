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

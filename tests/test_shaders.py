import os
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from waw2bo2 import shaders


FILM = '''ps_3_0
def c0, 0.299, 0.587, 0.114, 1
dcl_texcoord v0.xy
dcl_2d s0
texld_pp r0, v0, s0
dp3_pp r0.w, r0, c0
mad r0.xyz, r0, c7.w, r0.w
mov r1.xyz, c6
mad r1.xyz, r1, r0.w, c5
mad_pp oC0.xyz, r0, r1, c7
mov oC0.w, c0.w
'''


class ShaderTests(unittest.TestCase):
    def test_binding_is_required_for_every_external_register(self):
        with self.assertRaisesRegex(shaders.ShaderError, 'constant binding'):
            shaders.translate(FILM, {})
        contract = {'constants': {f'c{i}': {'buffer': 2, 'index': i-5} for i in (5,6,7)},
                    'samplers': {'s0': {'texture': 3, 'sampler': 4}},
                    'inputs': {'v0': {'semantic': 'TEXCOORD1', 'width': 2}},
                    'outputs': {'oC0': {'semantic': 'SV_Target0'}}}
        source, report = shaders.translate(FILM, contract)
        self.assertEqual(report['binding_status'], 'explicit_contract')
        self.assertIn('register(b2)', source)
        self.assertIn('register(t3)', source)
        self.assertIn('register(s4)', source)
        self.assertIn('t6_cb2[2]', source)
        if os.name == 'nt':
            self.assertEqual(shaders.compile_hlsl(source, 'ps_5_0')[:4], b'DXBC')

    def test_unknown_opcode_and_relative_addressing_fail_closed(self):
        for instruction in ('call l0', 'mova a0.x, c0.x', 'mov oC0, c[a0.x]'):
            with self.subTest(instruction=instruction), self.assertRaises(shaders.ShaderError):
                shaders.translate('ps_3_0\n' + instruction + '\nmov oC0, c0')

    def test_resource_limits_and_interface_truncation_rejected(self):
        contract = {'constants': {f'c{i}': {'buffer': 2, 'index': i-5} for i in (5,6,7)},
                    'samplers': {'s0': {'texture': 128, 'sampler': 0}},
                    'inputs': {'v0': {'semantic': 'TEXCOORD0', 'width': 1}},
                    'outputs': {'oC0': {'semantic': 'SV_Target0'}}}
        with self.assertRaisesRegex(shaders.ShaderError, 'resource binding'):
            shaders.translate(FILM, contract)
        contract['samplers']['s0']['texture'] = 0
        with self.assertRaisesRegex(shaders.ShaderError, 'truncates'):
            shaders.translate(FILM, contract)

    @unittest.skipUnless(os.name == 'nt', 'native D3D compiler requires Windows')
    def test_stage_preserves_first_source_and_records_failure(self):
        good = shaders.compile_hlsl('float4 main() : COLOR { return float4(1,0,0,1); }', 'ps_3_0')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, data in (('first', good), ('second', b'invalid')):
                (root/name/'shader_bin').mkdir(parents=True)
                (root/name/'shader_bin/a.cso').write_bytes(data)
            (root/'first/shader_bin/bad.cso').write_bytes(b'invalid')
            report = shaders.stage([root/'first', root/'second'], root/'out')
            self.assertEqual((report['translated'], report['unsupported']), (1, 1))
            self.assertEqual((root/'out/a.cso').read_bytes()[:4], b'DXBC')
            self.assertFalse((root/'out/bad.cso').exists())
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '1'}):
                serial = shaders.stage([root/'first', root/'second'], root/'serial')
            self.assertEqual(serial, report)
            self.assertEqual({p.name: p.read_bytes() for p in (root/'out').iterdir()},
                             {p.name: p.read_bytes() for p in (root/'serial').iterdir()})

    def test_flow_must_be_balanced_and_typed(self):
        for instructions in ('if_gt c0.x, c0.y\nmov oC0, c0',
                             'defi i0, 2, 0, 0, 0\nrep i0\nendif\nmov oC0, c0'):
            with self.assertRaises(shaders.ShaderError):
                shaders.translate('ps_3_0\n' + instructions)

    def test_aliasing_saturation_and_absolute_source(self):
        hlsl, _ = shaders.translate('ps_3_0\ndef c0, 1, -2, 3, 4\nmov r0, c0\nmov_sat r0.xy, r0_abs.yxzw\nmov oC0, r0')
        self.assertIn('float4 value = saturate(abs((r0).yxzw)); r0.xy = value.xy;', hlsl)
        if os.name == 'nt':
            shaders.compile_hlsl(hlsl, 'ps_5_0')

    @unittest.skipUnless(os.name == 'nt', 'native D3D compiler requires Windows')
    def test_actual_vertex_bytecode_roundtrip(self):
        original = shaders.compile_hlsl('''float4 main(float4 position : POSITION) : POSITION {
            return position * float4(1,2,3,1);
        }''', 'vs_3_0')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)/'input.cso'
            source.write_bytes(original)
            report = shaders.translate_file(source, Path(tmp)/'converted')
            self.assertEqual(report['target_profile'], 'vs_5_0')
            compiled = (Path(tmp)/'converted.cso').read_bytes()
            self.assertEqual(compiled[:4], b'DXBC')
            self.assertIn('vs_5_0', shaders.disassemble(compiled))

    @unittest.skipUnless(os.name == 'nt', 'native D3D compiler requires Windows')
    def test_repeat_projective_sampling_and_discard_compile(self):
        hlsl, _ = shaders.translate('''ps_3_0
def c0, 1, 0.5, 0, 1
defi i0, 2, 0, 0, 0
dcl_texcoord v0
dcl_2d s0
mov r0, c0
rep i0
texldp r1, v0, s0
add r0, r0, r1
endrep
texkill r0
mov oC0, r0
''')
        shaders.compile_hlsl(hlsl, 'ps_5_0')

    @unittest.skipUnless(os.name == 'nt', 'native D3D compiler requires Windows')
    def test_unsupported_translation_does_not_emit_bytecode(self):
        original = shaders.compile_hlsl('''float4 values[32];
            float4 main(float4 position : POSITION) : POSITION {
                return values[(int)position.x];
            }''', 'vs_3_0')
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)/'input.cso'
            source.write_bytes(original)
            output = Path(tmp)/'out'
            with self.assertRaises(shaders.ShaderError):
                shaders.translate_file(source, output)
            for suffix in ('.cso', '.hlsl', '.json'):
                self.assertFalse(output.with_suffix(suffix).exists())


if __name__ == '__main__':
    unittest.main()

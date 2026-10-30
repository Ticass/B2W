import ctypes
import json
import sys
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import nativeprobes, shaders, shaderruntime


@unittest.skipUnless(sys.platform == 'win32', 'requires the native D3D compiler')
class NativeProbeTests(unittest.TestCase):
    def code(self):
        return shaders.compile_hlsl('''
TextureCube<float4> reflectionProbeSampler : register(t15);
SamplerState ss : register(s15);
float4 main(float3 p:TEXCOORD0):SV_Target0 {
 return reflectionProbeSampler.SampleLevel(ss,p,0) + 0.1;
}''', 'ps_5_0')

    def test_checksum_and_native_validation(self):
        code = self.code()
        self.assertEqual(nativeprobes.checksum(code[20:]), code[4:20])
        adapted = nativeprobes.adapt(code, 15)
        self.assertNotEqual(adapted, code)
        self.assertEqual(nativeprobes.checksum(adapted[20:]), adapted[4:20])
        self.assertEqual(shaderruntime.resource_names(shaders.disassemble(code)),
                         shaderruntime.resource_names(shaders.disassemble(adapted)))
        assembly = shaders.disassemble(adapted)
        self.assertIn('mul r0.xyz, r0.xyzw, r0.wwww', assembly)
        self.assertIn('mul r0.xyz, r0.xyzw, r0.xyzw', assembly)
        self.assertIn('mov r0.w, l(1.000000)', assembly)
        fn = ctypes.WinDLL('d3dcompiler_47.dll').D3DStripShader
        fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
        fn.restype = ctypes.c_long
        out = ctypes.c_void_p()
        self.assertEqual(fn(adapted, len(adapted), 2, ctypes.byref(out)), 0)
        self.assertTrue(shaders._blob_bytes(out))

    def test_native_material_gets_isolated_program_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stock, project = root/'stock', root/'project'
            (stock/'techniquesets').mkdir(parents=True)
            (stock/'shader_bin').mkdir()
            code = self.code()
            (stock/'shader_bin/ps_native.cso').write_bytes(code)
            (stock/'techniquesets/native.json').write_text(json.dumps({
                'name':'native', 'techniques':[{'name':'native_4','passArray':[
                    {'pixelShader':{'name':'native'},'customSamplerFlags':1}]}]}))
            material = {'techniqueSet':'native'}
            self.assertEqual(nativeprobes.bind(material,project,stock), 1)
            self.assertTrue(material['techniqueSet'].startswith('waw/runtime_'))
            data = json.loads((project/'techniquesets'/f'{material["techniqueSet"]}.json').read_text())
            p = data['techniques'][0]['passArray'][0]
            self.assertEqual(p['customSamplerFlags'], 1)
            self.assertTrue(p['pixelShader']['name'].startswith('waw/native_probe_'))
            self.assertEqual(nativeprobes.bind(material,project,stock), 0)
            self.assertEqual((stock/'shader_bin/ps_native.cso').read_bytes(), code)

    def test_wrong_resource_fails_closed(self):
        with self.assertRaisesRegex(ValueError, 'sample not found'):
            nativeprobes.adapt(self.code(), 14)

    def test_stock_gameplay_materials_are_adapted_without_replacing_source_overlay(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stock, dump, overlay = root/'stock', root/'dump', root/'overlay'
            (stock/'techniquesets').mkdir(parents=True)
            (stock/'shader_bin').mkdir()
            (stock/'shader_bin/ps_native.cso').write_bytes(self.code())
            (stock/'techniquesets/native.json').write_text(json.dumps({
                'name': 'native', 'techniques': [{'passArray': [
                    {'pixelShader': {'name': 'native'}, 'customSamplerFlags': 1}]}]}))
            for name in ('knife', 'blood', 'source'):
                p = dump/f'materials/mc/{name}.json'
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(json.dumps({'techniqueSet': 'native', 'textures': [{'image': name}]}))
            source = overlay/'materials/mc/source.json'
            source.parent.mkdir(parents=True)
            source.write_text('{"techniqueSet":"waw/runtime_source"}')
            changed = nativeprobes.bind_native_materials(dump, overlay, stock)
            self.assertEqual(set(changed), {'materials/mc/knife.json', 'materials/mc/blood.json'})
            self.assertEqual(json.loads(source.read_text())['techniqueSet'], 'waw/runtime_source')
            for name in ('knife', 'blood'):
                material = json.loads((overlay/f'materials/mc/{name}.json').read_text())
                self.assertTrue(material['techniqueSet'].startswith('waw/runtime_'))
                self.assertEqual(material['textures'], [{'image': name}])
            self.assertEqual(nativeprobes.bind_native_materials(dump, overlay, stock), {})


if __name__ == '__main__':
    unittest.main()

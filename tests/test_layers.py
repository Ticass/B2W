import struct
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import fbx, shaders, shaderruntime as runtime, world


class LayerFormatTests(unittest.TestCase):
    def test_waw_vertex_colour_is_d3dcolor(self):
        # WaW D3DCOLOR 0xAARRGGBB: an orange lava tint stays orange (T6 reads RGBA)
        r, g, b, a = world._color(0x80FF8000)
        self.assertEqual((r, g, b), (1.0, 128 / 255, 0.0))
        self.assertAlmostEqual(a, 128 / 255)

    def test_world_vertex_formats_and_waw_strides(self):
        # MTL_WORLDVERT_TEX_<t>_NRM_<n>: same enum in WaW and T6
        self.assertEqual(world.layer_format(0), (1, 1))
        self.assertEqual(world.layer_format(1), (2, 1))
        self.assertEqual(world.layer_format(2), (2, 2))
        self.assertEqual(world.layer_format(5), (3, 3))
        self.assertEqual(world.layer_format(11), (5, 3))
        # WaW: float2 texcoords, 4-byte normal transforms
        self.assertEqual(world.layer_stride(0), 0)
        self.assertEqual(world.layer_stride(1), 8)
        self.assertEqual(world.layer_stride(2), 12)
        self.assertEqual(world.layer_stride(3), 16)
        with self.assertRaises(world.FormatError):
            world.layer_format(12)

    def test_layer_data_becomes_named_fbx_uv_sets(self):
        surface = world.Surface(0, 2, 1, 0, '*blend', 0, 0, 1, 0, (0, 0, 0), (1, 1, 1),
                                layer_data_offset=4, world_vert_format=2)
        records = struct.pack('<2f4B', 0.25, 0.75, 0xFF, 0x80, 0x80, 0xFF) + struct.pack('<2f4B', 1.5, -2.0, 1, 2, 3, 4)
        vertex = world.Vertex((0, 0, 0), (1, 1, 1, 1), (0, 0), (0, 0), (0, 0, 1), (1, 0, 0), 1.0)
        gfx = world.GfxWorld('m', 'm', '', [vertex, vertex], [0, 1, 1], [surface], [], layer_data=b'pad!' + records)
        sets = fbx._layer_uv_sets(gfx, {0: 0, 1: 1}, {0: surface, 1: surface}, 2)
        self.assertEqual([name for name, _ in sets], ['LayerUV1', 'LayerNormal1'])
        self.assertEqual(sets[0][1], [0.25, 0.25, 1.5, 3.0])  # V flipped like the base UV
        self.assertEqual(sets[1][1], [0xFF + 256 * 0x80, 0x80 + 256 * 0xFF, 1 + 512, 3 + 1024])
        self.assertEqual(fbx._layer_uv_sets(gfx, {0: 0}, {0: surface}, 0), [])


class NeutralAdapterTests(unittest.TestCase):
    def test_neutral_input_and_constant_texel_need_no_resources(self):
        vs = 'vs_3_0\ndcl_position v0\ndcl_blendweight v1\ndcl_position o0\ndcl_texcoord o1\nmov o0, v0\nmov o1, v1\n'
        contract = {'inputs': {'v0': {'semantic': 'POSITION0', 'width': 4},
                               'v1': {'semantic': 'BLENDWEIGHT0', 'width': 4, 'adapter': 'waw_neutral',
                                      'constant': [0.0, 0.0, 0.0, 0.0]}},
                    'outputs': {'o0': {'semantic': 'SV_Position', 'width': 4}, 'o1': {'semantic': 'TEXCOORD0', 'width': 4}},
                    'constants': {}, 'samplers': {}}
        hlsl, _ = shaders.translate(vs, contract)
        self.assertNotIn('BLENDWEIGHT', hlsl)
        self.assertIn('v1 = float4(0.0, 0.0, 0.0, 0.0);', hlsl)
        shaders.compile_hlsl(hlsl, 'vs_5_0')
        ps = 'ps_3_0\ndcl_texcoord v0\ndcl_2d s6\ntexld r0, v0, s6\nmov oC0, r0\n'
        contract = {'inputs': {'v0': {'semantic': 'TEXCOORD0', 'width': 4}},
                    'outputs': {'oC0': {'semantic': 'SV_Target0', 'width': 4}}, 'constants': {},
                    'samplers': {'s6': {'texture': 0, 'sampler': 0, 'constant': [0.0, 0.0, 0.0, 0.0]}}}
        hlsl, _ = shaders.translate(ps, contract)
        self.assertNotIn('tex_s6', hlsl)
        shaders.compile_hlsl(hlsl, 'ps_5_0')

    def test_scorch_programs_are_recognized_by_technique(self):
        self.assertTrue(runtime.is_scorch_program({'techniqueSet': 'wc_l_sm_r0c0n0s0_sco'}))
        self.assertFalse(runtime.is_scorch_program({'techniqueSet': 'wc_l_sm_r0c0n0s0'}))


class AddedBindingTests(unittest.TestCase):
    PS = '// Resource Bindings:\n// colorMapSampler texture float4 2d t0 1\ndcl_sampler s0, mode_default\n'

    def _pass(self):
        return {'perPrimArgCount': 1, 'perObjArgCount': 1, 'stableArgCount': 3, 'args': [
            {'type': 3, 'location': 0, 'buffer': 3, 'size': 64, 'u': {'value': 1}},
            {'type': 3, 'location': 576, 'buffer': 0, 'size': 64, 'u': {'value': 2}},
            {'type': 2, 'location': 0, 'buffer': 0, 'size': 1, 'u': {'value': runtime.t6_hash('normalMap')}},
            {'type': 2, 'location': 0x101, 'buffer': 0, 'size': 1, 'u': {'value': runtime.t6_hash('colorMap')}},
            {'type': 5, 'location': 1232, 'buffer': 0, 'size': 16, 'u': {'value': 16777216}}]}

    def test_added_material_texture_keeps_hash_order_in_pass_and_material(self):
        native = self._pass()
        material = {'textures': [{'name': 'normalMap', 'image': 'n'}, {'name': 'colorMap', 'image': 'c'}]}
        source = {'textures': [{'name': 'specularMap', 'image': 's', 'semantic': 'specularMap'}]}
        arg = runtime.add_material_texture(native, material, source, runtime.t6_hash('specularMap'), self.PS)
        hashes = [a['u']['value'] for a in native['args'] if a['type'] == 2]
        self.assertEqual(hashes, sorted(hashes))
        self.assertEqual([t['name'] for t in material['textures']],
                         sorted(['normalMap', 'colorMap', 'specularMap'], key=runtime.t6_hash))
        self.assertEqual(native['stableArgCount'], 4)
        self.assertNotIn(arg['location'] & 255, (0, 1, 13, 14, 15))
        self.assertIsNone(runtime.add_material_texture(native, material, source, runtime.t6_hash('detailMap'), self.PS))

    def test_code_texture_goes_after_material_textures(self):
        native = self._pass()
        slot = runtime.add_code_texture(native, runtime.CODE_TEXTURE_ARGS['attenuationSampler'], self.PS)
        types = [a['type'] for a in native['args'][2:]]
        self.assertEqual(types, [2, 2, 4, 5])
        self.assertEqual(native['args'][4]['location'], slot | (slot << 8))
        self.assertEqual(runtime.add_code_texture(native, 15, self.PS), slot)  # bound once


class TechsetVertexFormatTests(unittest.TestCase):
    def test_world_vertex_format_from_waw_technique_names(self):
        from waw2bo2 import techsets
        self.assertEqual(techsets.world_vert_format('wc_l_sm_r0c0'), 0)
        self.assertEqual(techsets.world_vert_format('l_sm_r0c0_b1c1'), 1)            # TEX_2_NRM_1
        self.assertEqual(techsets.world_vert_format('l_sm_r0c0n0s0_b1c1n1s1_sco'), 2)  # TEX_2_NRM_2
        self.assertEqual(techsets.world_vert_format('l_sm_r0c0_b1c1_b2c2'), 3)       # TEX_3_NRM_1
        self.assertEqual(techsets.world_vert_format('l_sm_r0c0_b1c1_b2c2_b3c3'), 6)  # TEX_4_NRM_1

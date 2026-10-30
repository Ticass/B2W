import unittest
from unittest.mock import patch
from waw2bo2 import techsets


class HudTechniqueTests(unittest.TestCase):
    def test_four_layer_source_can_use_native_streams_with_original_blending(self):
        source = 'l_sm_r0c0n0s0_sco_b1c1_b2c2_b3c3'
        candidates = ['lit_sm_r0c0n0s0_b1c1n1x1_m2c2_m3c3',
                      'lit_sm_r0c0n0s0_b1c1n1x1_b2c2n2x2']
        with patch.object(techsets, 'LAYERED_VERTEX_DATA', True):
            result = techsets.match(source, candidates, source_programs=True)
            self.assertIn('m3c3', result.target)
            self.assertFalse(any('layer 3 dropped' in n for n in result.notes))
            self.assertTrue(any('source program supplies' in n for n in result.notes))
            fallback = techsets.match(source, candidates)
            self.assertTrue(any('layer 3 dropped' in n for n in fallback.notes))

    def test_rope_uses_measured_native_rope_material_stream(self):
        result = techsets.match('m_rope', ['mc_lit_sm_r0c0_77e21qq8'])
        self.assertEqual(result.target, 'mc_lit_sm_r0c0_77e21qq8')
        with self.assertRaises(techsets.TechsetError):
            techsets.match('m_rope', ['wpc_lit_sm_r0c0_12345678'])

    def test_same_family_canopy_and_shadowcaster_are_not_rejected(self):
        self.assertEqual(techsets.match('mc_treecanopy', ['mc_treecanopy_4ze496f7']).target,
                         'mc_treecanopy_4ze496f7')
        self.assertEqual(techsets.match('wc_shadowcaster', ['wpc_shadowcaster_wj6w5j60']).target,
                         'wpc_shadowcaster_wj6w5j60')

    def test_2d_hud_uses_trivial_not_world_shader(self):
        result = techsets.match("2d", ["wc_unlit_blend_a1b2c3d4", "trivial_9z33feqw"])
        self.assertEqual(result.target, "trivial_9z33feqw")
        with self.assertRaises(techsets.TechsetError):
            techsets.match("2d", ["wc_unlit_blend_a1b2c3d4"])

    def test_unruled_techniques_get_generic_fallbacks(self):
        self.assertEqual(techsets.last_resort('mc_ambient_t0c0_dfoliage_sco')[0], 'mc_l_sm_t0c0')
        self.assertEqual(techsets.last_resort('wc_unlit_falloff_add')[0], 'wc_unlit_blend')
        self.assertEqual(techsets.last_resort('wc_water')[0], 'wc_l_sm_b0c0')
        self.assertEqual(techsets.last_resort('mc_ambient_r0c0_dfoliage_sco')[0], 'mc_l_sm_r0c0')
        self.assertEqual(techsets.last_resort('wc_l_sm_r0c0'), [])
        # the fallbacks go through the ordinary rules to real T6 donors
        self.assertEqual(techsets.match('mc_l_sm_t0c0', ['mc_lit_sm_t0c0_4ze496f7']).target, 'mc_lit_sm_t0c0_4ze496f7')
        self.assertEqual(techsets.match('wc_unlit_blend', ['wpc_unlit_blend_2840z6q0']).target,
                         'wpc_unlit_blend_2840z6q0')

    def test_donor_thermal_variant_is_not_inherited(self):
        donor = {"techniqueSet": "mc_lit_sm_r0c0_x", "thermalMaterial": "mc/mtl_t6_wpn_zmb_raygun2_1_thermal",
                 "textures": [{"name": "colorMap", "semantic": "color", "image": "stock"}]}
        out = techsets.build_material({"textures": [{"name": "colorMap", "image": "waw_body"}]}, donor, [])
        self.assertNotIn("thermalMaterial", out)
        self.assertEqual(out["textures"][0]["image"], "waw_body")

    def test_two_sided_source_keeps_both_faces(self):
        donor = {"techniqueSet": "mc_lit_sm_r0c0_x", "textures": [],
                 "stateBits": [{"cullFace": "back", "colorWriteRgb": False},
                               {"cullFace": "back", "colorWriteRgb": True}]}
        two_sided = {"stateBits": [{"cullFace": "back", "colorWriteRgb": False},
                                   {"cullFace": "none", "colorWriteRgb": True}]}
        out = techsets.build_material(two_sided, donor, [])
        self.assertEqual([s["cullFace"] for s in out["stateBits"]], ["none", "none"])
        one_sided = {"stateBits": [{"cullFace": "back", "colorWriteRgb": True}]}
        out = techsets.build_material(one_sided, donor, [])
        self.assertEqual([s["cullFace"] for s in out["stateBits"]], ["back", "back"])
        self.assertEqual(donor["stateBits"][0]["cullFace"], "back")    # the donor is not modified

    def test_wireframe_pass_does_not_make_a_two_sided_source_one_sided(self):
        # wc/chainlink_fence: every drawn pass is cull none; the debug wireframe
        # pass (technique 53, polymodeLine) culls back and is never drawn.
        donor = {"techniqueSet": "wc_lit_sm_t0c0n0s0", "textures": [],
                 "stateBits": [{"cullFace": "back", "colorWriteRgb": True}]}
        fence = {"stateBits": [{"cullFace": "none", "colorWriteRgb": True},
                               {"cullFace": "back", "colorWriteRgb": True, "polymodeLine": True}]}
        out = techsets.build_material(fence, donor, [])
        self.assertEqual([s["cullFace"] for s in out["stateBits"]], ["none"])
        wire_only = {"stateBits": [{"cullFace": "none", "colorWriteRgb": True, "polymodeLine": True}]}
        out = techsets.build_material(wire_only, donor, [])
        self.assertEqual([s["cullFace"] for s in out["stateBits"]], ["back"])

    def test_no_marks_follows_the_source(self):
        # wc/water_sumpf_muddy_green: WaW gameFlags 4,10; the donor had 2,10, so
        # blood decals stuck to the water surface.
        donor = {"techniqueSet": "wc_water_x", "textures": [], "gameFlags": ["2", "10"]}
        out = techsets.build_material({"gameFlags": ["4", "10"]}, donor, [])
        self.assertEqual(out["gameFlags"], ["2", "10", "NO_MARKS"])
        marked = dict(donor, gameFlags=["NO_MARKS", "10"])
        self.assertEqual(techsets.build_material({"gameFlags": ["10"]}, marked, [])["gameFlags"], ["10"])


if __name__ == "__main__":
    unittest.main()

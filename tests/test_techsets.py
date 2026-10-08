import unittest
from waw2bo2 import techsets


class HudTechniqueTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()

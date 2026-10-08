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


if __name__ == "__main__":
    unittest.main()

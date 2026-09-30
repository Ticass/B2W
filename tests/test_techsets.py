import unittest
from waw2bo2 import techsets


class HudTechniqueTests(unittest.TestCase):
    def test_2d_hud_uses_trivial_not_world_shader(self):
        result = techsets.match("2d", ["wc_unlit_blend_a1b2c3d4", "trivial_9z33feqw"])
        self.assertEqual(result.target, "trivial_9z33feqw")
        with self.assertRaises(techsets.TechsetError):
            techsets.match("2d", ["wc_unlit_blend_a1b2c3d4"])


if __name__ == "__main__":
    unittest.main()

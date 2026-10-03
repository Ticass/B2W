import math
import os
import unittest

from waw2bo2 import glow, shaders, visions

KNEEDEEP = '''r_glow "1"
r_glowRadius0 "5"
r_glowBloomCutoff "0.26"
r_glowBloomDesaturation "0"
r_glowBloomIntensity0 "1"
r_glowRayIntensity "0"
r_filmEnable "1"
r_filmContrast "1.05"
r_filmBrightness "0"
r_filmDesaturation "0.1"
r_filmLightTint "1.37921 1.36135 1.22803"
r_filmDarkTint "0.818935 0.807322 0.84079"
'''


def waw_kernel(sigma, pairs=8):
    """Python mirror of CoDWaW sub_74A550 (same-size target): pair weights."""
    k = -0.5 / (sigma * sigma)
    weights = []
    for i in range(pairs):
        we = math.exp((2 * i) ** 2 * k) * (0.5 if i == 0 else 1.0)
        wo = math.exp((2 * i + 1) ** 2 * k)
        weights.append(we + wo)
    norm = 0.5 / sum(weights)
    weights = [w * norm for w in weights]
    count = pairs
    for i in range(pairs - 1, -1, -1):
        if weights[i] < 0.01:
            count = i + 1
    return weights[:count]


class GlowTest(unittest.TestCase):
    def test_vision_constants_follow_waw_glow_setup(self):
        fields = visions.parse(KNEEDEEP)
        constants, notes = glow.glow_constants(fields)
        self.assertEqual(notes, [])
        self.assertAlmostEqual(constants['cutoff'], 0.26)
        self.assertAlmostEqual(constants['cut_scale'], 1 / 0.74)
        self.assertEqual(constants['intensity'], 1.0)
        off, _ = glow.glow_constants(visions.parse('r_glow "0"\nr_glowBloomIntensity0 "1"\nr_glowRadius0 "5"\n'))
        self.assertIsNone(off)
        _, rays = glow.glow_constants(visions.parse(KNEEDEEP.replace('r_glowRayIntensity "0"', 'r_glowRayIntensity "1"')))
        self.assertTrue(rays)

    def test_overlay_film_matches_vision_lut_film(self):
        fields = visions.parse(KNEEDEEP)
        film = glow.film_constants(fields)
        x = (0.2, 0.5, 0.8)
        lum = sum(a * b for a, b in zip(x, glow.LUMA))
        tint = [d + (l - d) * lum for d, l in zip(film['dark'], film['light'])]
        mine = [max(0., min(1., ((1 - film['desat']) * c + film['desat'] * lum) * t * film['scale'] + film['bias']))
                for c, t in zip(x, tint)]
        for a, b in zip(mine, visions.film(x, fields)):
            self.assertAlmostEqual(a, b, places=6)

    def test_kernel_truncation_matches_waw(self):
        # 1080p, r_glowRadius0 5: sigma = 5 * 0.25 * 1080 / 480
        self.assertEqual(len(waw_kernel(2.8125)), 5)
        self.assertEqual(len(waw_kernel(1.875)), 4)

    @unittest.skipUnless(os.name == 'nt', 'd3dcompiler')
    def test_overlay_programs_compile(self):
        fields = visions.parse(KNEEDEEP)
        constants, _ = glow.glow_constants(fields)
        for g in (constants, None):
            self.assertEqual(shaders.compile_hlsl(glow.pixel_hlsl(glow.film_constants(fields), g), 'ps_5_0')[:4], b'DXBC')
        self.assertIn('return x;', glow.pixel_hlsl(glow.film_constants({'r_filmenable': '0'}), None))

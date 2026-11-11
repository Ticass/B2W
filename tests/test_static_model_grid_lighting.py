import unittest

from waw2bo2 import lighting


class _Tracer:
    def __init__(self, blocked=()):
        self.blocked = set(blocked)

    def visible(self, start, end):
        return tuple(round(c) for c in end) not in self.blocked


SPOT = {"type": lighting.LIGHT_SPOT, "origin": [0.0, 0.0, 100.0], "dir": [0.0, 0.0, 1.0], "radius": 200.0,
        "cosHalfFovOuter": 0.7071, "rotationLimit": 1.0, "index": 2}


class StaticModelGridLightingTests(unittest.TestCase):
    """WaW static model grid lighting (sub_72D520 -> sub_71D440)."""

    def test_spot_volume_faces_minus_dir(self):
        # WaW sub_5953C0 tests dot(dir, light - point) > 0: the spot shines along -dir
        self.assertTrue(lighting._in_light_volume(SPOT, [0.0, 0.0, 0.0]))
        self.assertFalse(lighting._in_light_volume(SPOT, [0.0, 0.0, 200.0]))
        self.assertFalse(lighting._in_light_volume(SPOT, [150.0, 0.0, 90.0]))  # outside the cone
        self.assertFalse(lighting._in_light_volume(SPOT, [0.0, 0.0, -150.0]))  # outside the radius
        self.assertTrue(lighting._in_light_volume({"type": lighting.LIGHT_SUN}, [0.0, 0.0, 0.0]))
        self.assertFalse(lighting._in_light_volume({"type": lighting.LIGHT_NONE}, [0.0, 0.0, 0.0]))

    def test_dominant_light_prefers_a_real_light(self):
        corners = [(0, 0.6, [0, 0, 0], (0, 0, 0)), (1, 0.3, [0, 0, 64], (0, 255, 0)),
                   (2, 0.1, [0, 32, 0], (0, 7, 0))]
        light, kept = lighting._dominant_light(corners, _Tracer(), [0, 0, 0])
        self.assertEqual(light, 7)
        self.assertEqual(len(kept), 3)

    def test_blocked_trace_corner_drops_once_one_is_reachable(self):
        # corner 1 has its needsTrace bit (1 << 1) and its trace is blocked
        corners = [(0, 0.5, [0, 0, 0], (0, 3, 0)), (1, 0.5, [0, 0, 64], (0, 5, 0b10))]
        tracer = _Tracer(blocked={(0, 0, 63)})
        light, kept = lighting._dominant_light(corners, tracer, [0, 0, 0])
        self.assertEqual(light, 3)
        self.assertEqual([c[0] for c in kept], [0])

    def test_light_weight_below_threshold_is_ignored(self):
        corners = [(0, 0.0005, [0, 0, 0], (0, 9, 0)), (1, 0.9995, [0, 0, 64], (0, 4, 0))]
        light, kept = lighting._dominant_light(corners, _Tracer(), [0, 0, 0])
        self.assertEqual((light, len(kept)), (4, 1))


if __name__ == "__main__":
    unittest.main()

from pathlib import Path
import json
from types import SimpleNamespace
import struct
import tempfile
import unittest

from waw2bo2 import lighting


class LightingTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "grid.bin"
            path.write_bytes(data)
            return lighting.read_grid(path)

    def fixture(self):
        return (lighting.HEADER.pack(b"W2BLGRD1", 1, 1, 1, 10, 20, 30, 11, 22, 33,
                                     0, 1, 2, 20, 2, 1)
                + struct.pack("<2H", 0, 0xFFFF)
                + struct.pack("<4HI", 20, 3, 30, 4, 0) + bytes((1, 0, 2, 1, 2, 0, 0, 0))
                + struct.pack("<HBB", 0, 3, 1) * 2 + bytes(range(168)))

    def test_preserves_palette_entry_flags_and_rows(self):
        grid = self.read(self.fixture())
        self.assertEqual(grid.row_starts, (0, 0xFFFF))
        self.assertEqual(grid.entries, struct.pack("<HBB", 0, 3, 1) * 2)
        self.assertEqual(grid.colors, bytes(range(168)))
        self.assertEqual(grid.report()["entries"], 2)
        self.assertEqual(list(grid.points()), [((10, 21, 32), 0), ((10, 22, 32), 1)])

    def test_rejects_truncation_trailing_data_bad_palette(self):
        fixture = self.fixture()
        bad_palette = bytearray(fixture)
        struct.pack_into("<H", bad_palette, lighting.HEADER.size + 24, 1)
        for data in (fixture[:15], fixture[:-1], fixture + b"extra", bad_palette):
            with self.subTest(length=len(data)), self.assertRaises(lighting.LightingError):
                self.read(data)

    def test_runtime_translation_preserves_row_addresses_and_decodes_gamma_units(self):
        with tempfile.TemporaryDirectory() as temp:
            src, dst = Path(temp) / "source.bin", Path(temp) / "target.bin"
            src.write_bytes(self.fixture())
            report = lighting.stage_grid(src, dst)
            target = dst.read_bytes()
            self.assertEqual(target[:8], b"W2BT6LG1")
            self.assertEqual(target[lighting.HEADER.size:lighting.HEADER.size + 24],
                             src.read_bytes()[lighting.HEADER.size:lighting.HEADER.size + 24])
            self.assertEqual(target[lighting.HEADER.size + 24:lighting.HEADER.size + 32],
                             struct.pack("<HBB", 0, 3, 255) * 2)
            palette = target[-168:]
            for source_byte, target_byte in zip(range(168), palette):
                source_gamma = 2 * source_byte / 255
                target_gamma = (32 * (target_byte / 255) ** 2) ** 0.5
                self.assertAlmostEqual(source_gamma, target_gamma, delta=0.012)
            self.assertEqual(report["source_trace_mask_points"], 2)

    def test_primary_light_types_and_definitions_are_translated(self):
        with tempfile.TemporaryDirectory() as temp:
            source, target = Path(temp) / "source.json", Path(temp) / "target.json"
            local = {"radius": 400.0, "cosHalfFovOuter": 0.5, "cosHalfFovInner": 0.75, "canUseShadowMap": 1}
            source.write_text(json.dumps({"lights": [
                {"type": 1, "defName": "", "color": [0.5, 0.5, 0.5], "canUseShadowMap": 0},
                {"type": 2, "defName": "point", "color": [2.0, 0.5, 0.0], "exponent": 2, "index": 1, **local},
                {"type": 3, "defName": "point", "exponent": 0, "index": 2, **local}]}))
            unshadowed = []
            self.assertEqual(lighting.stage_primary_lights(source, target, unshadowed), {"point"})
            lights = json.loads(target.read_text())["lights"]
            self.assertEqual([light["type"] for light in lights], [1, 2, 5])
            self.assertEqual(lights[2]["defName"], "waw_light/point")
            # WaW gamma light colour -> T6 linear (hdr scale 1); the sun is set from SunParse
            self.assertEqual(lights[1]["color"], [4.0, 0.25, 0.0])
            self.assertEqual(lights[0]["color"], [0.5, 0.5, 0.5])
            # no primary-light shadow data is converted: local lights draw unshadowed
            self.assertEqual([light["canUseShadowMap"] for light in lights], [0, 0, 0])
            self.assertEqual(unshadowed, [1, 2])
            self.assertNotIn("falloff", lights[0])
            self.assertEqual(lights[1]["falloff"], [1.0, 400.0, 1.0, 0.0])
            self.assertEqual(lights[1]["aAbB"], [0.75, 0.5, 0.5, 0.0])
            self.assertEqual(lights[2]["aAbB"][2], 1e30)
            # neither 0 (SPOT_SQUARE) nor 1 (SPOT_ROUND): T6 draws them as plain spots
            self.assertEqual(lights[1]["roundness"], 0.5)

    def test_t6_light_fields_reproduce_waw_light_uniforms(self):
        # Replays T6 sub_782FA0 on the staged fields; WaW sub_742400 values result.
        light = {"radius": 700.0, "cosHalfFovOuter": 0.5, "cosHalfFovInner": 0.766, "exponent": 1}
        fields = lighting.t6_light_fields(light)
        f, ab = fields["falloff"], fields["aAbB"]
        x0, x1 = f[0], max(f[2], f[0])
        if abs(x1 - x0) <= 0.00024414062:
            x1 = x0 + 0.00024414062
        y0, y1 = f[1], min(f[3], f[1])
        fall_a = (1 / (ab[0] - ab[1]), 1 / (ab[2] - ab[3]), 1 / (x1 - x0), 1 / (y1 - y0))
        fall_b = (-ab[1] * fall_a[0], -ab[3] * fall_a[1], -x0 * fall_a[2], -y0 * fall_a[3])
        self.assertAlmostEqual(-fall_a[3], 1 / 700.0)  # WaW lightPosition.w
        spread = 1 / (0.766 - 0.5)
        self.assertAlmostEqual(fall_a[0], spread)  # WaW lightSpotFactors
        self.assertAlmostEqual(fall_b[0], -0.5 * spread)
        self.assertAlmostEqual(fall_a[1], 1.0)
        # T6's own window: 1 - d/radius inside the radius, after the near ramp
        window = lambda d: min(max(d * fall_a[2] + fall_b[2], 0), 1) * min(max(d * fall_a[3] + fall_b[3], 0), 1)
        self.assertAlmostEqual(window(350.0), 0.5)
        self.assertEqual(window(700.0), 0.0)
        self.assertGreater(fields["dAttenuation"] / 700.0 ** 2, 1.0)

    def test_surface_merge_preserves_distinct_primary_lights(self):
        from waw2bo2.fbx import merge_surfaces
        surfaces = [SimpleNamespace(material="wall", lightmap_index=0,
                    primary_light_index=light, mins=(0, 0, 0), maxs=(1, 1, 1),
                    base_index=0, triangle_count=1) for light in (1, 2, 1)]
        meshes = merge_surfaces(SimpleNamespace(surfaces=surfaces, indices=[0, 1, 2]))
        self.assertEqual(len(meshes), 2)
        self.assertEqual(sorted(len(mesh[1]) for mesh in meshes), [1, 2])
        for _, mesh, _ in meshes:
            self.assertEqual(len({surface.primary_light_index for surface in mesh}), 1)

    def test_invalid_runs_do_not_reach_native_loader(self):
        fixture = self.fixture()
        for offset, replacement in ((lighting.HEADER.size, b'\xfe\xff'),
                                    (lighting.HEADER.size + 16, b'\x00'),
                                    (lighting.HEADER.size + 20, b'\xff')):
            bad = bytearray(fixture)
            bad[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaises(lighting.LightingError):
                self.read(bad)


if __name__ == "__main__":
    unittest.main()

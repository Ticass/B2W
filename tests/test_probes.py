import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import iwi, probes
from waw2bo2.fbx import merge_surfaces


def cube_dds(size, texel_bgra):
    """Uncompressed 32-bit BGRA cube, one level (the T4 unlinker's layout)."""
    header = bytearray(128)
    header[:4] = b'DDS '
    struct.pack_into('<7I', header, 4, 124, 0x1007, size, size, size * 4, 0, 1)
    struct.pack_into('<2I4s5I', header, 76, 32, 0x41, b'\0\0\0\0', 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    struct.pack_into('<4I', header, 108, 0x1008, 0xFE00, 0, 0)
    return bytes(header) + bytes(texel_bgra) * (size * size * 6)


def source(origins, cells=None, smodels=()):
    return probes.SourceProbes(list(origins), [f'*reflection_probe{i}' for i in range(len(origins))],
                               cells or [], list(smodels))


class ProbePlanTests(unittest.TestCase):
    def test_within_the_limit_every_baked_probe_keeps_its_index(self):
        src = source([(0, 0, 0), (10, 0, 0), (20, 0, 0)])
        plan = probes.plan(src, {}, {1, 2})
        self.assertEqual(plan.kept, [0, 1, 2])
        self.assertEqual([plan.t6_index(i) for i in range(3)], [0, 1, 2])

    def test_probe_zero_and_unbaked_probes_stay_neutral(self):
        plan = probes.plan(source([(0, 0, 0), (10, 0, 0), (20, 0, 0)]), {}, {0, 2})
        self.assertEqual(plan.kept, [0, 2])
        self.assertEqual([plan.t6_index(i) for i in range(3)], [0, 0, 1])

    def test_over_the_t6_limit_dropped_probes_use_their_nearest_kept_probe(self):
        # 40 probes in two tight rows: 31 kept, the rest share a neighbour
        origins = [(0, 0, 0)] + [(100.0 * (i % 20), 1000.0 * (i // 20), 0) for i in range(40)]
        src = source(origins)
        plan = probes.plan(src, {i: 1 for i in range(41)}, set(range(1, 41)))
        self.assertEqual(len(plan.kept), probes.T6_MAX_PROBES)
        self.assertEqual(plan.kept[0], 0)
        for i in range(1, 41):
            t6 = plan.t6_index(i)
            self.assertTrue(1 <= t6 < probes.T6_MAX_PROBES)
            nearest = min(plan.kept[1:], key=lambda k: (sum((a - b) ** 2 for a, b in zip(origins[i], origins[k])), k))
            self.assertEqual(plan.kept[t6], nearest if i not in plan.kept else i)

    def test_reduction_is_deterministic(self):
        origins = [(0, 0, 0)] + [(float(i * 37 % 101), float(i * 53 % 89), float(i % 7)) for i in range(60)]
        a = probes.plan(source(origins), {i: i % 5 for i in range(61)}, set(range(1, 61)))
        b = probes.plan(source(origins), {i: i % 5 for i in range(61)}, set(range(1, 61)))
        self.assertEqual(a, b)


class ProbeImageTests(unittest.TestCase):
    def test_cube_holds_waw_display_value_rgb_times_alpha(self):
        # BGRA bytes: b=50 g=100 r=200 a=51 (0.2)
        out = iwi.dds_to_iwi(cube_dds(4, (50, 100, 200, 51)), rgba_texels=probes._display_texels)
        self.assertEqual(out[:4], b'IWi\x1b')
        fmt, flags = out[4], out[5]
        self.assertEqual(fmt, iwi.FMT_RGBA)
        self.assertTrue(flags & iwi.FLAG_CUBEMAP)
        texel = out[64:68]
        self.assertEqual(tuple(texel), (40, 20, 10, 255))
        self.assertEqual(len(out), 64 + 4 * 4 * 4 * 6)


class ProbeStageTests(unittest.TestCase):
    def test_stage_assigns_surface_and_model_probes_and_writes_the_plan(self):
        surfaces = [SimpleNamespace(reflection_probe_index=i, material='m', lightmap_index=0, primary_light_index=1,
                                    brush_model=0, mins=(0, 0, 0), maxs=(1, 1, 1), base_index=0, triangle_count=0)
                    for i in (0, 1, 2, 2)]
        models = [SimpleNamespace(reflection_probe_index=0) for _ in range(2)]
        world = SimpleNamespace(surfaces=surfaces, static_models=models, indices=[])
        report = SimpleNamespace(warnings=[], errors=[], images=[], content={})
        src = source([(0, 0, 0), (1, 2, 3), (4, 5, 6)], smodels=[2, 0])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / 'src'
            images.mkdir()
            for i in range(3):
                (images / f'_reflection_probe{i}.dds').write_bytes(cube_dds(2, (0, 0, 255, 85)))
            plan = probes.stage(report, world, src, [images], root / 'out')
            written = json.loads((root / 'out/BSP/reflectionprobes.json').read_text())
            self.assertTrue((root / 'out/images/reflection_probe2.iwi').is_file())
            self.assertFalse((root / 'out/images/reflection_probe0.iwi').exists())  # neutral, from stage_images
        self.assertEqual(report.errors, [])
        self.assertEqual(plan.kept, [0, 1, 2])
        self.assertEqual([s.reflection_probe_index for s in surfaces], [0, 1, 2, 2])
        self.assertEqual([m.reflection_probe_index for m in models], [2, 0])
        self.assertEqual(written['probes'][1], {'origin': [1, 2, 3], 'image': 'reflection_probe1'})
        self.assertEqual(written['cellProbes'], [1, 2])
        # each probe is its own mesh: meshes never mix probes
        self.assertEqual(sorted(len(group) for _m, group, _l in merge_surfaces(world)), [1, 1, 2])

    def test_without_a_probe_dump_the_world_keeps_the_neutral_probe(self):
        report = SimpleNamespace(warnings=[], errors=[], images=[], content={})
        self.assertIsNone(probes.stage(report, SimpleNamespace(surfaces=[], static_models=[]), None, [], Path('.')))
        self.assertTrue(any('reflection probes' in w for w in report.warnings))


if __name__ == '__main__':
    unittest.main()

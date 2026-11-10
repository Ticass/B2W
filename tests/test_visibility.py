import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from waw2bo2 import visibility
from waw2bo2.fbx import merge_surfaces


def surface(index, cell_centre, brush_model=0):
    x = cell_centre
    return SimpleNamespace(index=index, material='m', lightmap_index=0, primary_light_index=0, reflection_probe_index=0,
                           brush_model=brush_model, shadow_copy=False, cell=-1, base_index=0, triangle_count=1,
                           mins=(x, 0.0, 0.0), maxs=(x + 1, 1.0, 1.0))


def dump():
    # plane 0 is x = 100: its front side (left child) is cell 1, behind is cell 0;
    # the third leaf of the tree is solid
    return {'cellCount': 2, 'planes': [[1, 0, 0, 100, 0, 0], [0, 0, 1, 500, 2, 0]], 'nodes': [3, 3, 2, 4, 3, 0, 1],
            'cells': [{'mins': [-1000, 0, 0], 'maxs': [100, 1, 1], 'surfaces': [0], 'smodels': [0], 'portals': []},
                      {'mins': [100, 0, 0], 'maxs': [1000, 1, 1], 'surfaces': [1], 'smodels': [0, 1], 'portals': []}]}


def world(*surfaces, models=2):
    return SimpleNamespace(surfaces=list(surfaces), indices=[0, 1, 2], visibility=None,
                           static_models=[SimpleNamespace(origin=(200.0, 0.0, 0.0)) for _ in range(models)])


def report():
    return SimpleNamespace(warnings=[], content={})


class VisibilityTest(unittest.TestCase):
    def test_point_tree_follows_the_engine_node_stream(self):
        data = dump()
        self.assertEqual(visibility.cell_at(data, (200, 0, 0)), 1)
        self.assertEqual(visibility.cell_at(data, (0, 0, 0)), 0)
        self.assertEqual(visibility.cell_at(data, (0, 0, 600)), -1)

    def test_surfaces_of_two_cells_never_merge(self):
        w = world(surface(0, 0.0), surface(1, 200.0))
        self.assertEqual(len(merge_surfaces(w)), 1)
        visibility.assign(report(), w, dump())
        self.assertEqual([s.cell for s in w.surfaces], [0, 1])
        self.assertEqual(len(merge_surfaces(w)), 2)

    def test_unlisted_geometry_joins_the_cell_around_it(self):
        data = dump()
        data['cells'][0]['surfaces'] = []
        data['cells'][1]['smodels'] = [0]
        w = world(surface(0, 0.0), surface(1, 200.0), surface(2, 0.0, brush_model=3))
        r = report()
        visibility.assign(r, w, data)
        self.assertEqual([s.cell for s in w.surfaces], [0, 1, -1])
        self.assertIn(1, data['cells'][1]['smodels'])
        self.assertEqual(r.content['visibility']['placed_by_position'], 2)

    def test_a_cell_lists_its_kept_probes_only(self):
        plan = SimpleNamespace(t6_index=lambda source: {5: 2, 6: 2, 7: 0}.get(source, 1))
        w = world(surface(0, 0.0), surface(1, 200.0))
        data = dump()
        visibility.assign(report(), w, data, SimpleNamespace(cells=[[5, 6, 7], []]), plan)
        self.assertEqual(data['cells'][0]['probes'], [2])
        self.assertEqual(data['cells'][1]['probes'], [])

    def test_no_dump_or_a_bad_one_links_one_cell_and_says_so(self):
        for data in (None, {'cells': []}, {**dump(), 'cells': dump()['cells'] * 200}):
            w = world(surface(0, 0.0), surface(1, 200.0))
            r = report()
            visibility.assign(r, w, data)
            self.assertIsNone(w.visibility)
            self.assertTrue(any('one cell' in text for text in r.warnings))

    def test_written_cells_hold_mesh_indices(self):
        w = world(surface(0, 0.0), surface(1, 200.0))
        r = report()
        visibility.assign(r, w, dump())
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'visibility.json'
            visibility.write(r, w, {0: 1, 1: 0, 2: -1}, path)
            cells = json.loads(path.read_text())['cells']
        self.assertEqual([c['meshes'] for c in cells], [[1], [0]])
        self.assertEqual(cells[1]['smodels'], [0, 1])

    def test_a_stale_file_goes_when_the_map_has_no_cells(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'visibility.json'
            path.write_text('{}')
            visibility.write(report(), world(), {}, path)
            self.assertFalse(path.exists())


if __name__ == '__main__':
    unittest.main()

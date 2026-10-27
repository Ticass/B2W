import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import t6bridge, techsets


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data))


def files(root):
    return {path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob('*') if path.is_file()}


class MaterialPoolTests(unittest.TestCase):
    def stage(self, root, workers):
        source, native, project = root / 'source', root / 'native', root / f'project{workers}'
        names = {f'm{i:03d}' for i in range(t6bridge.PARALLEL_MATERIALS + 6)}
        for name in names:
            write_json(source / techsets.oat_material_path(name), {'techniqueSet': '2d', 'textures': [
                {'name': 'colorMap', 'image': f'{name}_col', 'semantic': 'colorMap'}]})
        write_json(native / 'materials/hud.json', {'techniqueSet': 'trivial_12345678', 'textures': [
            {'name': 'colorMap', 'image': 'white', 'semantic': 'colorMap'}]})
        write_json(native / 'techniquesets/trivial_12345678.json', {'techniques': []})
        # a BO2 equivalent among them is staged on the calling process, in order
        equivalent = native / 'materials/equivalent.json'
        write_json(equivalent, {'techniqueSet': 'trivial_12345678', 'textures': []})
        names.add('m050x')
        report = t6bridge.StageReport('any_map')
        with patch.dict(os.environ, {'WAW2BO2_WORKERS': str(workers)}):
            used = t6bridge.stage_materials(report, names, [source], native / 'materials', project, native,
                                            native_fallbacks={'m050x': equivalent})
        return used, report, files(project)

    def test_worker_processes_match_a_serial_run(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            serial = self.stage(root, 1)
            pooled = self.stage(root, 2)
        self.assertEqual(serial[0], pooled[0])
        self.assertEqual(serial[1].materials, pooled[1].materials)
        self.assertEqual(serial[1].warnings, pooled[1].warnings)
        self.assertEqual(serial[1].errors, pooled[1].errors)
        self.assertEqual(serial[2], pooled[2])
        names = [m['name'] for m in pooled[1].materials]
        self.assertEqual(names, sorted(names))
        self.assertIn('m050x', names)

    def test_batches_restore_the_staging_process_state(self):
        before = techsets.LAYERED_VERTEX_DATA, t6bridge.shaders.SHARED_COMPILES
        context = {'layered_vertex_data': not before[0], 'shared_compiles': Path('shared'),
                   'roots': [], 'rename': {}, 'native': frozenset(), 'native_fallbacks': frozenset()}
        with patch.object(t6bridge, '_stage_waw_material', return_value=None) as convert:
            t6bridge._material_batch(context, ['a'])
        convert.assert_called_once()
        self.assertEqual((techsets.LAYERED_VERTEX_DATA, t6bridge.shaders.SHARED_COMPILES), before)


if __name__ == '__main__':
    unittest.main()

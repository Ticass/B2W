import json
import operator
import os
from pathlib import Path
import struct
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from waw2bo2 import audio, iwi, lightmaps, parallel, t6bridge


def dds(width, height, pixels):
    header = bytearray(128)
    header[:4] = b'DDS '
    struct.pack_into('<7I', header, 4, 124, 0x100F, height, width, 0, 1, 1)
    struct.pack_into('<2I4s5I', header, 76, 32, 0x41, bytes(4), 32,
                     0xFF, 0xFF00, 0xFF0000, 0xFF000000)
    return bytes(header) + pixels


def files(root):
    return {path.relative_to(root).as_posix(): path.read_bytes()
            for path in root.rglob('*') if path.is_file()}


class ParallelTests(unittest.TestCase):
    def test_thread_jobs_overlap_and_return_in_input_order(self):
        barrier = threading.Barrier(2, timeout=5)
        def job(value):
            barrier.wait()  # A serial implementation cannot complete this.
            return value * 2
        with patch.dict(os.environ, {'WAW2BO2_WORKERS': '2'}):
            self.assertEqual(list(parallel.ordered_map(job, [3, 1], label='Test')), [6, 2])

    def test_spawned_processes_execute_picklable_cpu_jobs(self):
        with patch.dict(os.environ, {'WAW2BO2_WORKERS': '2'}):
            self.assertEqual(list(parallel.ordered_map(operator.neg, [2, 3, 4],
                             label='Test', processes=True)), [-2, -3, -4])

    def test_worker_failure_propagates_and_invalid_counts_are_rejected(self):
        def fail(value):
            raise RuntimeError('asset failed')
        with patch.dict(os.environ, {'WAW2BO2_WORKERS': '2'}):
            with self.assertRaisesRegex(RuntimeError, 'asset failed'):
                list(parallel.ordered_map(fail, [1, 2], label='Test'))
        with patch.dict(os.environ, {'WAW2BO2_WORKERS': '0'}):
            with self.assertRaises(ValueError):
                parallel.worker_count(3)

    def test_lightmap_processes_preserve_every_output_byte_and_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for index in range(3):
                pixels = bytes([20 + index, 64, 128, 200]) * (32 * 64)
                (root / f'_lightmap{index}_secondary.dds').write_bytes(dds(32, 64, pixels))
            world = SimpleNamespace(surfaces=[SimpleNamespace(material='wall', lightmap_index=i)
                                             for i in range(3)])
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '1', 'WAW2BO2_DIAG_PAGEID': '1'}):
                serial = lightmaps.stage(world, [root], root / 'serial', {'wall'})
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '2', 'WAW2BO2_DIAG_PAGEID': '1'}):
                concurrent = lightmaps.stage(world, [root], root / 'parallel', {'wall'})
            self.assertEqual(concurrent, serial)
            self.assertEqual(files(root / 'serial'), files(root / 'parallel'))

    def test_parallel_audio_preserves_pcm_reports_and_conflict_detection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entries = []
            for name, sample in (('a.wav', 100), ('b.wav', 200), ('a.bin', 300)):
                (root / name).write_bytes(audio.canonical_pcm(struct.pack('<h', sample) * 10, 1, 48000))
                entries.append({'name': name, 'output': name, 'loadType': 0})
            (root / 'sounds.stage.json').write_text(json.dumps({'audio': entries}))
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '1'}):
                serial = audio.stage(root, root / 'serial')
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '2'}):
                concurrent = audio.stage(root, root / 'parallel')
            # The output folder appears in the conflict error; compare semantics.
            self.assertEqual(concurrent['audio'], serial['audio'])
            self.assertEqual(len(concurrent['errors']), 1)
            self.assertIn('conflicting PCM output', concurrent['errors'][0]['reason'])
            self.assertEqual((root / 'serial/a.wav').read_bytes(), (root / 'parallel/a.wav').read_bytes())
            self.assertEqual((root / 'parallel/a.wav').read_bytes(), (root / 'a.wav').read_bytes())

    def test_parallel_images_preserve_source_precedence_and_filename_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, color in (('_a', 100), ('b', 200)):
                (root / f'{name}.dds').write_bytes(dds(4, 4, bytes([color, 20, 30, 255]) * 16))
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '1'}):
                serial = t6bridge.StageReport('test')
                serial_names = t6bridge.stage_images(serial, [root], root / 'serial',
                    iwd_dirs=[], extra_images=['*a', '_a', 'b'])
            with patch.dict(os.environ, {'WAW2BO2_WORKERS': '2'}):
                concurrent = t6bridge.StageReport('test')
                names = t6bridge.stage_images(concurrent, [root], root / 'parallel',
                    iwd_dirs=[], extra_images=['*a', '_a', 'b'])
            self.assertEqual(names, serial_names)
            self.assertEqual(concurrent.images, serial.images)
            self.assertEqual(concurrent.errors, serial.errors)
            self.assertEqual(files(root / 'serial'), files(root / 'parallel'))


if __name__ == '__main__':
    unittest.main()

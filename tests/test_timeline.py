import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import timeline


class TimelineTests(unittest.TestCase):
    def test_spans_and_child_processes_nest_in_one_timeline(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ):
            target = Path(directory) / 'build_timeline.jsonl'
            timeline.start(target)
            with timeline.span('build') as counts:
                with timeline.span('step'):
                    subprocess.run([sys.executable, '-c', 'pass'], check=True)
                counts['assets'] = 3
            timeline.merge(target)
            records = timeline.load(target)
        by_name = {r['name']: r for r in records}
        process = next(r for r in records if r['kind'] == 'process')
        self.assertEqual(process['parent'], by_name['step']['id'])
        self.assertEqual(process['returncode'], 0)
        self.assertEqual(by_name['step']['parent'], by_name['build']['id'])
        self.assertEqual(by_name['build']['assets'], 3)
        summary = timeline.summarize(records)
        [root] = summary['tree']
        self.assertEqual(root['name'], 'build')
        self.assertEqual(root['children'][0]['children'][0]['kind'], 'process')
        self.assertGreaterEqual(root['unattributed'], 0)

    def test_failed_span_is_recorded_and_reraised(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ):
            target = Path(directory) / 't.jsonl'
            timeline.start(target)
            with self.assertRaises(ValueError):
                with timeline.span('broken'):
                    raise ValueError('boom')
            [record] = timeline.load(target)
        self.assertFalse(record['ok'])
        self.assertIn('boom', record['error'])

    def test_disabled_timeline_records_nothing(self):
        with patch.dict(os.environ, {timeline.ENV: ''}):
            with timeline.span('anything') as counts:
                counts['x'] = 1
            self.assertIsNone(timeline.path())


if __name__ == '__main__':
    unittest.main()

import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from waw2bo2 import progress


class ProgressTests(unittest.TestCase):
    def test_verbose_native_stream_is_also_preserved_in_tool_log(self):
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, WAW2BO2_VERBOSE='1'):
            console = io.StringIO()
            log = Path(folder) / 'tool.log'
            with contextlib.redirect_stdout(console):
                result = progress.native([sys.executable, '-c',
                                          "print('asset one'); raise SystemExit(7)"], log)
            self.assertEqual(result.returncode, 7)
            self.assertIn('asset one', console.getvalue())
            self.assertEqual(log.read_text().strip(), 'asset one')

    def test_verbose_capture_preserves_validation_output_and_failure(self):
        with patch.dict(os.environ, WAW2BO2_VERBOSE='1'), contextlib.redirect_stdout(io.StringIO()) as console:
            result = progress.captured([sys.executable, '-c',
                                       "print('ERROR: bad input'); raise SystemExit(3)"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 3)
        self.assertIn('ERROR: bad input', result.stdout)
        self.assertIn('ERROR: bad input', console.getvalue())

    def test_normal_progress_is_bounded_and_names_items_only_in_verbose(self):
        with patch.dict(os.environ, WAW2BO2_VERBOSE='0'), contextlib.redirect_stdout(io.StringIO()) as console:
            self.assertEqual(list(progress.items('Images', ['a', 'b'])), ['a', 'b'])
        self.assertIn('Images: 2/2 completed', console.getvalue())
        self.assertNotIn('Images 1/2: a', console.getvalue())
        with patch.dict(os.environ, WAW2BO2_VERBOSE='1'), contextlib.redirect_stdout(io.StringIO()) as console:
            list(progress.items('Images', ['a']))
        self.assertIn('Images 1/1: a', console.getvalue())

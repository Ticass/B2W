from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('discord_agent_pipeline', ROOT / 'tools/discord_agent_pipeline.py')
pipeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pipeline)


class PatchBoundaryTests(unittest.TestCase):
    def test_rejects_binary_and_protected_files(self):
        for stat in (b'-\t-\tsrc/waw2bo2/data.bin\0', b'1\t0\t.github/workflows/ci.yml\0',
                     b'1\t0\tsrc/waw2bo2/AGENTS.md\0', b'1\t0\ttools/discord_agent_pipeline.py\0',
                     b'1\t0\t../outside.py\0'):
            with self.subTest(stat=stat), patch.object(pipeline, 'git', return_value=stat):
                with self.assertRaises(ValueError):
                    pipeline.validate_patch(b'diff --git a/file b/file\n')

    def test_rejects_symlinks(self):
        with self.assertRaises(ValueError):
            pipeline.validate_patch(b'diff --git a/src/waw2bo2/a.py b/src/waw2bo2/a.py\nnew file mode 120000\n')

    def test_validates_a_real_patch_against_a_clean_git_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def git(*args, input=None):
                return subprocess.run(['git', '-C', str(root), *args], input=input,
                    check=True, capture_output=True).stdout
            git('init')
            git('config', 'user.name', 'Test')
            git('config', 'user.email', 'test@example.invalid')
            source = root / 'src/waw2bo2/example.py'
            source.parent.mkdir(parents=True)
            source.write_text('value = 1\n')
            git('add', '.')
            git('commit', '-m', 'fixture')
            source.write_text('value = 2\n')
            fix = git('diff', 'HEAD')
            git('restore', '.')
            with patch.object(pipeline, 'git', side_effect=git):
                self.assertEqual(pipeline.validate_patch(fix), ['src/waw2bo2/example.py'])

    def test_screenshot_fetch_rejects_non_discord_urls_before_network(self):
        for url in ('http://cdn.discordapp.com/attachments/1/a.png', 'https://127.0.0.1/a.png',
                    'https://cdn.discordapp.com.evil.invalid/attachments/1/a.png',
                    'https://secret@cdn.discordapp.com/attachments/1/a.png'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                pipeline.screenshot(url, Path('unused.png'))


if __name__ == '__main__':
    unittest.main()

import importlib.util
from pathlib import Path
import tempfile
import unittest

_PATH = Path(__file__).resolve().parents[1] / 'tools' / 'promote_stable.py'
_SPEC = importlib.util.spec_from_file_location('promote_stable', _PATH)
promote_stable = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(promote_stable)

CHANGELOG = '# Changelog\n\n## Unreleased\n\n- Fix A.\n- Fix B.\n\n## v0.2.14\n\n- Old fix.\n'


class PromoteStableTests(unittest.TestCase):
    def test_next_version_increments_highest_patch(self):
        tags = ['v0.2.9', 'v0.2.14', 'nightly-2026-10-09-1', 'v0.2.10', 'junk']
        self.assertEqual(promote_stable.next_version(tags), '0.2.15')
        with self.assertRaises(ValueError):
            promote_stable.next_version(['nightly-2026-10-09-1'])

    def test_release_changelog_moves_unreleased_entries(self):
        text = promote_stable.release_changelog(CHANGELOG, '0.2.15')
        self.assertEqual(text, '# Changelog\n\n## Unreleased\n\n## v0.2.15\n\n- Fix A.\n- Fix B.\n\n'
                               '## v0.2.14\n\n- Old fix.\n')

    def test_release_changelog_without_entries_records_maintenance(self):
        text = promote_stable.release_changelog('# Changelog\n\n## Unreleased\n\n## v0.2.14\n\n- Old.\n', '0.2.15')
        self.assertIn('## v0.2.15\n\n- Maintenance release', text)

    def test_release_changelog_without_section_uses_commit_subjects(self):
        text = promote_stable.release_changelog('# Changelog\n\n## v0.2.14\n\n- Old.\n', '0.2.15',
                                                ['Parallelize asset conversion', ''])
        self.assertEqual(text, '# Changelog\n\n## Unreleased\n\n## v0.2.15\n\n- Parallelize asset conversion\n\n'
                               '## v0.2.14\n\n- Old.\n')

    def test_sync_changelog_keeps_later_main_entries_unreleased(self):
        released = promote_stable.release_changelog(CHANGELOG, '0.2.15')
        shipped = promote_stable.released_entries(released, '0.2.15')
        main = CHANGELOG.replace('## Unreleased\n\n', '## Unreleased\n\n- Saturday fix.\n')
        text = promote_stable.sync_changelog(main, '0.2.15', shipped)
        self.assertEqual(text, '# Changelog\n\n## Unreleased\n\n- Saturday fix.\n\n## v0.2.15\n\n- Fix A.\n- Fix B.\n\n'
                               '## v0.2.14\n\n- Old fix.\n')
        self.assertEqual(promote_stable.sync_changelog(text, '0.2.15', shipped), text)

    def test_cli_release_sets_both_version_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'src' / 'waw2bo2').mkdir(parents=True)
            (root / 'pyproject.toml').write_text('[project]\nname = "x"\nversion = "0.2.14"\n', encoding='utf-8')
            (root / 'src' / 'waw2bo2' / '__init__.py').write_text('"""Doc."""\n\n__version__ = "0.2.14"\n', encoding='utf-8')
            (root / 'CHANGELOG.md').write_text(CHANGELOG, encoding='utf-8')
            self.assertEqual(promote_stable.main(['release', '0.2.15', '--root', str(root)]), 0)
            self.assertIn('version = "0.2.15"', (root / 'pyproject.toml').read_text(encoding='utf-8'))
            self.assertIn('__version__ = "0.2.15"', (root / 'src' / 'waw2bo2' / '__init__.py').read_text(encoding='utf-8'))
            self.assertIn('## v0.2.15', (root / 'CHANGELOG.md').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()

"""WaW Mod Tools source lookup (waw2bo2.wawsource)."""
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from waw2bo2 import wawsource


class WawSourceTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.waw = self.tmp / "waw"
        (self.waw / "main").mkdir(parents=True)
        (self.waw / "raw" / "fx" / "env" / "fire").mkdir(parents=True)
        (self.waw / "raw" / "fx" / "env" / "fire" / "fx_a.efx").write_text("iwfx 2\n", encoding="ascii")
        self.tools = self.tmp / "project" / "wawModTools"
        (self.tools / "bin").mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def test_locate_prefers_explicit_then_install_then_project_folder(self):
        explicit = self.tmp / "elsewhere"
        self.assertEqual(wawsource.locate_mod_tools(explicit, self.waw, self.tmp / "project"), explicit)
        self.assertIsNone(wawsource.locate_mod_tools(None, self.waw, self.tmp / "project"))
        (self.tools / "bin" / "linker_pc.exe").write_bytes(b"")
        self.assertEqual(wawsource.locate_mod_tools(None, self.waw, self.tmp / "project"), self.tools)
        (self.waw / "bin").mkdir()
        (self.waw / "bin" / "linker_pc.exe").write_bytes(b"")
        self.assertEqual(wawsource.locate_mod_tools(None, self.waw, self.tmp / "project"), self.waw)

    def test_raw_found_in_install_when_tools_have_none(self):
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / "unlinker.exe", self.tmp / "work")
        self.assertEqual(src.raw, self.waw / "raw")
        self.assertIsNotNone(src.source("fx", "env/fire/fx_a"))
        self.assertIsNone(src.source("fx", "env/fire/missing"))
        self.assertIsNone(src.compile("fx", "env/fire/missing"))
        self.assertIn(f"WaW linker {src.linker} missing", src.check())

    def test_material_source_is_checked_before_declaring_absence(self):
        material = self.waw / 'raw/materials/community/surface'
        material.parent.mkdir(parents=True)
        material.write_bytes(b'compiled raw material')
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / 'unlinker.exe', self.tmp / 'work')
        self.assertEqual(src.source('material', 'community/surface'), material)
        self.assertIsNone(src.source('material', 'community/missing'))

    def test_partial_tools_raw_does_not_hide_installed_model_sources(self):
        (self.tools / 'raw/fx').mkdir(parents=True)
        model = self.waw / 'raw/xmodel/window_damage'
        model.parent.mkdir(parents=True)
        model.write_bytes(b'original model')
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / 'unlinker.exe', self.tmp / 'work')
        self.assertEqual(src.raw, self.tools / 'raw')
        self.assertEqual(src.source('xmodel', 'window_damage'), model)
        preferred = self.tools / 'raw/xmodel/window_damage'
        preferred.parent.mkdir()
        preferred.write_bytes(b'authored revision')
        self.assertEqual(src.source('xmodel', 'window_damage'), preferred)

    def test_draw_family_material_looks_up_unprefixed_raw_source(self):
        material = self.waw / 'raw/materials/$default3d'
        material.parent.mkdir(parents=True)
        material.write_bytes(b'raw material')
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / 'unlinker.exe', self.tmp / 'work')
        self.assertEqual(src.source('material', 'mc/$default3d'), material)
        self.assertEqual(src.source('material', 'wc/$default3d'), material)
        exact = self.waw / 'raw/materials/mc/$default3d'
        exact.parent.mkdir()
        exact.write_bytes(b'explicit family material')
        self.assertEqual(src.source('material', 'mc/$default3d'), exact)

    def test_builtin_family_alias_keeps_original_material_and_image_binding(self):
        material = self.waw / 'raw/materials/$default3d'
        material.parent.mkdir(parents=True)
        material.write_bytes(b'raw material')
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / 'unlinker.exe', self.tmp / 'work')
        dump = self.tmp / 'dump'
        (dump / 'materials').mkdir(parents=True)
        original = dump / 'materials/$default3d.json'
        original.write_text('{"techniqueSet":"default","textures":[{"image":"default"}]}')
        compile_asset = src.compile
        with patch.object(src, 'compile', return_value=dump) as compile_source:
            self.assertEqual(compile_asset('material', 'mc/$default3d'), dump)
        compile_source.assert_called_once_with('material', '$default3d')
        self.assertEqual((dump / 'materials/mc/$default3d.json').read_bytes(), original.read_bytes())

    def test_linker_failure_is_an_error_not_an_asset(self):
        # a "linker" that writes no fastfile
        linker = self.tools / "bin" / "linker_pc.exe"
        linker.write_bytes(b"")
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / "unlinker.exe", self.tmp / "work")
        with self.assertRaises((wawsource.WawSourceError, OSError)):
            src.compile("fx", "env/fire/fx_a")

    def test_fastfile_without_requested_asset_cannot_reuse_a_stale_dump(self):
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / 'unlinker.exe', self.tmp / 'work')
        zone = 'w2bsrc_fx_env_fire_fx_a'
        ws = src.work / 'linker'
        for folder in ('bin', 'zone_source', 'zone/english'):
            (ws / folder).mkdir(parents=True, exist_ok=True)
        stale = src.work / zone / 'fx/env/fire/fx_a.w2bfx.json'
        stale.parent.mkdir(parents=True)
        stale.write_text('{}')
        def fake_run(command, **kwargs):
            if command[0] == str(src.linker):
                (ws / f'zone/english/{zone}.ff').write_bytes(b'incomplete fastfile')
            class Result:
                returncode = 0
            return Result()
        with patch.object(src, '_workspace', return_value=ws), patch.object(wawsource.subprocess, 'run', side_effect=fake_run):
            with self.assertRaisesRegex(wawsource.WawSourceError, 'produced no fx'):
                src.compile('fx', 'env/fire/fx_a')
        self.assertFalse(stale.exists())
        self.assertFalse((src.work / zone / wawsource.DUMP_MARKER).exists())


if __name__ == "__main__":
    unittest.main()

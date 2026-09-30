"""WaW Mod Tools source lookup (waw2bo2.wawsource)."""
import tempfile
import unittest
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

    def test_linker_failure_is_an_error_not_an_asset(self):
        # a "linker" that writes no fastfile
        linker = self.tools / "bin" / "linker_pc.exe"
        linker.write_bytes(b"")
        src = wawsource.WawSourceAssets(self.tools, self.waw, self.tmp / "unlinker.exe", self.tmp / "work")
        with self.assertRaises((wawsource.WawSourceError, OSError)):
            src.compile("fx", "env/fire/fx_a")


if __name__ == "__main__":
    unittest.main()

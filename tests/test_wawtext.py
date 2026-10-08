import tempfile
import unittest
from pathlib import Path

from waw2bo2 import wawtext, weapons


class WawTextTests(unittest.TestCase):
    def test_windows_code_page_weapon_file_reads(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "ray_gun"
            path.write_bytes("WEAPONFILE\\displayName\\Ray Gun ×2\\damage\\100".encode("cp1252"))
            self.assertEqual(weapons.read_info_file(path), {"displayName": "Ray Gun ×2", "damage": "100"})

    def test_newlines_survive_write_read_cycles(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "aa12"
            fields = {"notetrackSoundMap": "lift waw/lift\nclipout waw/clipout"}
            for _ in range(3):
                path.write_text(weapons.write_info(fields), encoding="utf-8")  # CRLF on Windows
                fields = weapons.read_info_file(path)
            self.assertEqual(fields["notetrackSoundMap"], "lift waw/lift\nclipout waw/clipout")

    def test_utf8_stays_utf8(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.gsc"
            path.write_text("// café", encoding="utf-8")
            self.assertEqual(wawtext.read(path), "// café")


class SourceZoneNameTests(unittest.TestCase):
    def test_long_names_fit_the_waw_linker_limit_and_stay_unique(self):
        from waw2bo2 import wawsource
        self.assertEqual(wawsource.source_zone_name("material", "wc/brick"), "w2bsrc_material_wc_brick")
        a = wawsource.source_zone_name("material", "wc/peleliu_terrain_asphalt_runway_crack_blend_noscorch")
        b = wawsource.source_zone_name("material", "wc/peleliu_terrain_asphalt_runway_crack_blend_nomarks")
        self.assertLessEqual(len(a), wawsource.ZONE_NAME_LIMIT)
        self.assertNotEqual(a, b)


if __name__ == "__main__":
    unittest.main()

import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from waw2bo2 import t6bridge, waterbullets

WAW_TABLE = ("BULLET_PEN_TABLE\\small_wood\\72\\small_water\\96\\medium_water\\128\\large_water\\128"
             "\\large_wood\\192")


def _weapon(folder: Path, name: str, **fields) -> None:
    body = "WEAPONFILE" + "".join(f"\\{k}\\{v}" for k, v in fields.items())
    (folder / name).write_text(body, encoding="utf-8")


class WaterBulletsTest(unittest.TestCase):
    def test_depths_come_from_the_waw_table(self):
        self.assertEqual(waterbullets.water_depths(WAW_TABLE), {"small": 96.0, "medium": 128.0, "large": 128.0})

    def test_only_brushes_with_water_contents_count(self):
        clip = SimpleNamespace(brushes=[SimpleNamespace(contents=0x1), SimpleNamespace(contents=0x8000020)])
        self.assertTrue(waterbullets.has_water(clip))
        self.assertFalse(waterbullets.has_water(SimpleNamespace(brushes=[SimpleNamespace(contents=0x1)])))

    def test_rows_keep_hitscan_weapons_that_penetrate_water(self):
        with tempfile.TemporaryDirectory() as tmp:
            content = Path(tmp)
            (content / "weapons").mkdir()
            _weapon(content / "weapons", "zombie_shotgun", weaponType="bullet", penetrateType="medium", shotCount=8,
                    hipSpreadStandMin=4, hipSpreadMax=4, adsSpread=5.5)
            _weapon(content / "weapons", "ray_gun", weaponType="projectile", penetrateType="none", shotCount=0)
            _weapon(content / "weapons", "pistol_none", weaponType="bullet", penetrateType="none", shotCount=1)
            table = {"zombie_shotgun": "zombie_shotgun", "ray_gun": "ray_gun", "pistol_none": "pistol_none",
                     "missing": "missing_file"}
            rows = waterbullets.weapon_rows(content, table, waterbullets.water_depths(WAW_TABLE))
        self.assertEqual(list(rows), ["zombie_shotgun"])
        row = rows["zombie_shotgun"]
        self.assertEqual(row[0], 4.0)
        self.assertEqual(row[9], 5.5)
        self.assertEqual(row[10:], [8, 128.0])

    def test_source_continues_bullets_from_under_the_surface(self):
        source = waterbullets.water_bullets_source({"zombie_colt": [3, 2.5, 2, 6, 5, 4, 4, 1, 4.5, 0.2, 1, 96]})
        self.assertIn('level.waw_water_bullets["zombie_colt"] = waw_water_row( 3, 2.5, 2, 6, 5, 4, 4, 1, 4.5, 0.2, 1, 96 );',
                      source)
        self.assertIn('self waittill( "weapon_fired", weapon );', source)
        self.assertIn('trace["surfacetype"] != "water"', source)
        self.assertIn("magicbullet( weapon, start, start + dir * row[11], self );", source)
        self.assertNotIn("WEAPON_TABLE", source)

    def test_empty_source_is_a_no_op(self):
        source = waterbullets.water_bullets_source({})
        self.assertIn("init()\n{\n}", source)
        self.assertNotIn("weapon_fired", source)

    def test_stage_water_bullets_matches_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            table_path = root / waterbullets.PENETRATION_TABLE
            table_path.parent.mkdir()
            table_path.write_text(WAW_TABLE, encoding="utf-8")
            weapons = root / "content_source/weapons"
            weapons.mkdir(parents=True)
            _weapon(weapons, "shotgun", weaponType="bullet", penetrateType="medium", shotCount=8.5,
                    hipSpreadStandMin="bad", hipSpreadMax="1.345678", adsSpread="0.00001234")
            _weapon(weapons, "rocket", weaponType="projectile", penetrateType="medium")
            script = root / "maps/mp/waw/_waw2bo2_water_bullets.gsc"
            script.parent.mkdir(parents=True)
            names = {"source_gun": "shotgun", "duplicate": "shotgun", "source_rocket": "rocket"}
            for contents, roots in ((0x20, [root]), (0x20, []), (0x1, [root])):
                with self.subTest(contents=contents, roots=roots):
                    clip = SimpleNamespace(brushes=[SimpleNamespace(contents=contents)])
                    expected = t6bridge.StageReport(project="test")
                    with patch.dict(os.environ, {"WAW2BO2_NATIVE": "0"}):
                        t6bridge.stage_water_bullets(expected, clip, roots, root, names)
                    expected_script = script.read_bytes()
                    report = t6bridge.StageReport(project="test")
                    t6bridge.stage_water_bullets(report, clip, roots, root, names)
                    self.assertEqual(script.read_bytes(), expected_script)
                    self.assertEqual(report.content, expected.content)
                    self.assertEqual(report.warnings, expected.warnings)


if __name__ == "__main__":
    unittest.main()

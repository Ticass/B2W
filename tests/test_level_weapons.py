import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import entities, t6bridge


class _Stock:
    def __init__(self, index):
        self.index = index

    def zones_defining(self, kind, name):
        return sorted(z for z, names in self.index.items() if name in names)


class LevelWeaponTests(unittest.TestCase):
    def test_placed_weapons_and_turrets_are_named_and_stripped(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "entities.json"
            path.write_text(json.dumps({"entities": [
                {"classname": "worldspawn"},
                {"classname": "weapon_sw_357", "model": "weapon_mp_357magnum"},
                {"classname": "weapon_colt_dirty_harry"},
                {"classname": "misc_turret", "weaponinfo": "mg42_bipod_stand"},
                {"classname": "script_model", "model": "weapon_ger_mg42_bipod_mg"}]}))
            self.assertEqual(entities.placed_weapons(path), {"sw_357", "colt_dirty_harry", "mg42_bipod_stand"})
            self.assertEqual(entities.strip_weapons(path, {"colt_dirty_harry"}), 1)
            kept = [e["classname"] for e in json.loads(path.read_text())["entities"]]
            self.assertEqual(kept, ["worldspawn", "weapon_sw_357", "misc_turret", "script_model"])

    def test_weapons_from_the_zone_waw_loads_with_every_level_are_the_maps(self):
        # 420 places and includes colt_dirty_harry, which only common.ff defines
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "maps").mkdir()
            (root / "maps/nazi_zombie_420.gsc").write_text('main()\n{\n\tinclude_weapon( "colt_dirty_harry" );\n'
                                                           '\tINCLUDE_WEAPON("ray_gun");\n}\n')
            self.assertEqual(t6bridge.script_weapons([root]), {"colt_dirty_harry", "ray_gun"})
        stock = _Stock({"common": {"colt_dirty_harry"}, "nazi_zombie_factory": {"tesla_gun", "colt_dirty_harry"}})
        report = t6bridge.StageReport("zm_test")
        found = t6bridge.level_stock_weapons(report, {"colt_dirty_harry", "tesla_gun", "nowhere"}, stock)
        # another map's zone is not loaded with this one
        self.assertEqual(found, {"colt_dirty_harry"})
        self.assertTrue(any("WAW_STOCK_ASSET weapon colt_dirty_harry" in w for w in report.warnings))
        self.assertEqual(t6bridge.level_stock_weapons(report, {"colt_dirty_harry"}, None), set())


if __name__ == "__main__":
    unittest.main()

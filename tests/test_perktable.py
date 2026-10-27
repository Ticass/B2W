import json
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import perktable


class PerkTableTests(unittest.TestCase):
    def test_the_maps_perks_t6s_table_lacks_are_detected_from_the_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # adjacent table strings share separators; one follows data bytes
            exe = root / "t6zm.exe"
            exe.write_bytes(b"MZ\0specialty_armorvest\0specialty_quickrevive\0\x28\xc2specialty_grenadepulldeath\0"
                            b"xspecialty_notaperk\0")
            scripts = root / "project/maps/mp/waw"
            scripts.mkdir(parents=True)
            (scripts / "school.gsc").write_text('f()\n{\n\tif( self HasPerk( "specialty_Marathon" ) )\n\t\treturn;\n'
                                                '\tself SetPerk( "specialty_armorvest" );\n}\n')
            (root / "project/BSP").mkdir()
            (root / "project/BSP/entities.json").write_text(json.dumps({"entities": [
                {"classname": "trigger_use", "script_noteworthy": "specialty_boost"}]}))
            self.assertEqual(perktable.engine_perks(exe),
                             {"specialty_armorvest", "specialty_quickrevive", "specialty_grenadepulldeath"})
            result = perktable.stage(root / "project", exe)
            self.assertEqual(result["emulated"], ["specialty_boost", "specialty_marathon"])
            table = (scripts / perktable.SCRIPT).read_text()
            self.assertIn('level.waw2bo2_emulated_perks["specialty_marathon"] = 1;', table)
            self.assertNotIn("specialty_armorvest", table)

    def test_without_the_t6_executable_nothing_is_emulated_and_it_is_said(self):
        with tempfile.TemporaryDirectory() as tmp:
            scripts = Path(tmp) / "maps/mp/waw"
            scripts.mkdir(parents=True)
            original = perktable.t6_executable
            perktable.t6_executable = lambda: None
            try:
                result = perktable.stage(Path(tmp))
            finally:
                perktable.t6_executable = original
            self.assertIn("not found", result["error"])
            self.assertIn("level.waw2bo2_emulated_perks = [];", (scripts / perktable.SCRIPT).read_text())


if __name__ == "__main__":
    unittest.main()

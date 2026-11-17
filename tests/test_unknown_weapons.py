import json
import re
import unittest

from waw2bo2 import gscport


class UnknownWeaponTests(unittest.TestCase):
    """A mod script naming a weapon neither converted nor loaded by BO2 (e.g.
    "zombie_slipgun") must be skipped at runtime instead of aborting T6 with
    "unknown weapon"."""

    def test_loaded_weapon_table_is_lowercase_and_sorted(self):
        source = gscport.bo2_weapons_source({"Ray_Gun_zm", "none"})
        self.assertIn('level.waw2bo2_bo2_weapons["none"] = 1;', source)
        self.assertIn('level.waw2bo2_bo2_weapons["ray_gun_zm"] = 1;', source)
        self.assertLess(source.index('"none"'), source.index('"ray_gun_zm"'))

    def test_empty_table_is_a_valid_stub(self):
        self.assertIn("init()\n{\n}", gscport.bo2_weapons_source())

    def test_compat_checks_unconverted_names_against_loaded_weapons(self):
        compat = (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8")
        self.assertIn(r"maps\mp\waw\_waw2bo2_bo2weapons::init();", compat)
        body = re.search(r"\nwaw_weapon\( name \)\n\{(.*?)\n\}", compat, re.S).group(1)
        self.assertIn("level.waw2bo2_bo2_weapons[tolower( name )]", body)

    def test_weapon_name_builtins_route_through_compat(self):
        api = json.loads((gscport.COMPAT_DIR / "gsc_api.json").read_text(encoding="utf-8"))
        compat = (gscport.COMPAT_DIR / "_waw2bo2_compat.gsc").read_text(encoding="utf-8")
        for name in ("precacheitem", "weaponmaxammo", "weaponaltweaponname", "givestartammo"):
            self.assertIn(name, api["compat_wrappers"])
            self.assertRegex(compat, rf"\nwaw_{name}\(")


if __name__ == "__main__":
    unittest.main()

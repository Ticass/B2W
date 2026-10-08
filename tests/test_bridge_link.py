import unittest
from pathlib import Path

from waw2bo2 import t6bridge


class BridgeLinkCommandTests(unittest.TestCase):
    def test_map_assets_sort_before_stock_cache(self):
        # OAT orders search paths as sorted strings and opens the first match
        stage = Path("C:/Users/u/AppData/Local/WawConverter/builds/zm_x_1/stage")
        stock = Path("C:/Users/u/AppData/Local/WawConverter/builds/asset_cache/bo2/views/v")
        command = t6bridge.linker_command(Path("Linker.exe"), stage, "zm_x", stock)
        roots = command[command.index("--asset-search-path") + 1].split(";")
        self.assertEqual(roots[0], "?base?\\" + str(Path("zone_raw/zm_x")))
        self.assertEqual(roots[-1], str(stock))
        self.assertEqual(sorted(roots), roots)


if __name__ == "__main__":
    unittest.main()

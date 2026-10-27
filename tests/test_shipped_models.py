import tempfile
import unittest
from pathlib import Path

from waw2bo2 import t6bridge


class ShippedModelTests(unittest.TestCase):
    def test_every_model_the_maps_own_zones_ship_is_wanted(self):
        # Industrial Estate: zombie-hero viewhands chosen through arrays were
        # never converted; the map's zones ship them, so they convert
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'map/xmodel/sub').mkdir(parents=True)
            (root / 'map/xmodel/dempsey_viewhands.json').write_text('{}')
            (root / 'map/xmodel/sub/prop.json').write_text('{}')
            (root / 'patch').mkdir()
            self.assertEqual(t6bridge.shipped_models([root / 'map', root / 'patch', root / 'absent']),
                             {'dempsey_viewhands', 'sub/prop'})
            self.assertEqual(t6bridge.shipped_models([]), set())


if __name__ == '__main__':
    unittest.main()

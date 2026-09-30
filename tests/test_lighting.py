from pathlib import Path
import struct
import tempfile
import unittest

from waw2bo2 import lighting


class LightingTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "grid.bin"
            path.write_bytes(data)
            return lighting.read_grid(path)

    def fixture(self):
        return (lighting.HEADER.pack(b"W2BLGRD1", 1, 1, 1, 10, 20, 30, 11, 22, 33,
                                     0, 1, 2, 4, 1, 1)
                + struct.pack("<2H", 0, 2) + b"rows" + struct.pack("<HBB", 0, 3, 1) + bytes(range(168)))

    def test_preserves_palette_entry_flags_and_rows(self):
        grid = self.read(self.fixture())
        self.assertEqual(grid.row_starts, (0, 2))
        self.assertEqual(grid.entries, struct.pack("<HBB", 0, 3, 1))
        self.assertEqual(grid.colors, bytes(range(168)))
        self.assertEqual(grid.report()["entries"], 1)

    def test_rejects_truncation_trailing_data_bad_palette(self):
        fixture = self.fixture()
        bad_palette = bytearray(fixture)
        struct.pack_into("<H", bad_palette, lighting.HEADER.size + 8, 1)
        for data in (fixture[:15], fixture[:-1], fixture + b"extra", bad_palette):
            with self.subTest(length=len(data)), self.assertRaises(lighting.LightingError):
                self.read(data)


if __name__ == "__main__":
    unittest.main()

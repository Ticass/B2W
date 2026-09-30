"""Synthetic wavelet streams: no proprietary images/codebooks needed."""
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from waw2bo2 import iwi, wavelet

# A synthetic valid prefix code: 0 -> zero, 1 -> raw escape.
TABLES = (tuple(((-32768 if i & 1 else 0), 1) for i in range(4096)),) * 3


class Stream:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        self.bits.extend((value >> i) & 1 for i in range(n))

    def coefficient(self, value, colour=False):
        self.put(int(value != 0), 1)
        if value:
            self.put(value + (510 if colour else 255), 10 if colour else 9)

    def bytes(self):
        return bytes(sum(bit << j for j, bit in enumerate(self.bits[i:i + 8]))
                     for i in range(0, len(self.bits), 8))


def header(fmt=6, width=2, height=2, flags=0):
    return b"IWi\6" + struct.pack("<BB3H4I", fmt, flags, width, height, 1, 0, 0, 0, 0)


class WaveletTest(unittest.TestCase):
    def test_all_formats_transform_escape_and_parent_residual(self):
        for fmt, channels in [(6, 4), (7, 3), (8, 2), (9, 1), (10, 1)]:
            with self.subTest(fmt=fmt):
                s = Stream()
                for c in range(channels):
                    s.put(40 + 10 * c, 8)
                s.put(1, 1)  # update parent
                for c in range(channels):
                    s.coefficient(2)
                for c in range(channels):
                    s.put(1, 1)  # odd top-left correction
                    s.coefficient(2 if c == 0 or c == channels - 1 and channels != 3 else 0,
                                  colour=channels >= 3 and c in (1, 2))
                    s.coefficient(0, colour=channels >= 3 and c in (1, 2))
                    s.coefficient(0, colour=channels >= 3 and c in (1, 2))
                image = wavelet.decode(header(fmt) + s.bytes(), TABLES)
                stride = image.stride
                self.assertEqual(image.levels[1][:channels], bytes(40 + 10 * c for c in range(channels)))
                for c in range(channels):
                    mean = 42 + 10 * c
                    self.assertEqual(image.levels[0][c::stride], bytes((mean + 2, mean + 1, mean - 1, mean - 1)))
                if fmt == 7:
                    self.assertEqual(image.levels[0][3::4], b"\xff" * 4)
                parsed = iwi.read_dds(image.dds())
                self.assertEqual(parsed.iwi_format, fmt - 5)
                self.assertEqual(len(iwi.dds_to_iwi(image.dds())), 64 + 5 * channels)

    def test_rectangular_raw_mips_and_cube_shared_bits(self):
        for flags, faces in [(0, 1), (4, 6)]:
            s = Stream()
            # 4x2: raw 1x1, raw 2x1, then entropy 4x2, face interleaved.
            for size in (1, 2):
                for face in range(faces):
                    for _ in range(size):
                        s.put(30 + face, 8)
            for _face in range(faces):
                s.put(0, 1)
                for _block in range(2):
                    s.put(0, 1)
                    for _ in range(3):
                        s.coefficient(0)
            image = wavelet.decode(header(9, 4, 2, flags) + s.bytes(), TABLES)
            self.assertEqual(image.levels[0], b"".join(bytes([30 + f]) * 8 for f in range(faces)))
            self.assertEqual(iwi.read_dds(image.dds()).faces, faces)

    def test_reject_truncated_volume_and_missing_parent(self):
        for data in (b"", header() + b"\0", header(9, flags=8), header(9, flags=2) + b"\0" * 10):
            with self.assertRaises(iwi.IwiError):
                wavelet.decode(data, TABLES)

    def test_first_iwd_wins_and_stale_dds_not_used(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for folder in ("mod", "stock"):
                (root / folder).mkdir()
                with zipfile.ZipFile(root / folder / "a.iwd", "w") as z:
                    z.writestr("images/a.iwi", header(9, 1, 1, 2) + b"\x40")
            recovery = wavelet.IwdRecovery([root / "mod", root / "stock"], None, root / "out")
            self.assertEqual(recovery.entries['images/a.iwi'][0].parent.name, "mod")
            (root / "out" / "images").mkdir(parents=True)
            (root / "out" / "images" / "a.dds").write_bytes(b"stale")
            with self.assertRaises(iwi.IwiError):
                recovery.recover("a")


if __name__ == "__main__":
    unittest.main()

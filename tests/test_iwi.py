import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from waw2bo2 import iwi

def oat_load_iwi27(blob):
    """Python mirror of OAT iwi::LoadIwi27 size validation."""
    assert blob[:3] == b"IWi" and blob[3] == 27
    fmt, flags, w, h, d, _g = struct.unpack_from("<bbHHHf", blob, 4)
    sizes = struct.unpack_from("<8I", blob, 32)
    assert fmt in (1, 2, 3, 4, 5, 0xB, 0xC, 0xD, 0xE), fmt
    has_mips = not (flags & iwi.FLAG_NOMIPMAPS)
    faces = 6 if flags & iwi.FLAG_CUBEMAP else 1
    vol = bool(flags & iwi.FLAG_VOLMAP)
    count = iwi.full_mip_count(w, h, d if vol else 1) if has_mips else 1
    cur = 64
    for lvl in range(count - 1, -1, -1):
        cur += iwi.mip_size(fmt, w, h, d if vol else 1, lvl) * faces
        if lvl < 8:
            assert sizes[lvl] == cur, (lvl, sizes[lvl], cur)
    assert cur == len(blob), (cur, len(blob))
    return dict(fmt=fmt, flags=flags, w=w, h=h, d=d, mips=count, faces=faces)

def main(folder):
    for p in sorted(Path(folder).glob("*.dds")):
        blob = iwi.dds_to_iwi(p.read_bytes())
        print(p.name, oat_load_iwi27(blob))

if __name__ == "__main__":
    main(sys.argv[1])


import unittest


class LuminanceTests(unittest.TestCase):
    def test_waw_luminance_images_are_written_as_rgba_with_full_alpha(self):
        header = bytearray(128)
        header[:4] = b'DDS '
        struct.pack_into('<7I', header, 4, 124, 0x100F, 1, 4, 0, 1, 1)
        struct.pack_into('<2I4s5I', header, 76, 32, 0x20000, b'\0\0\0\0', 8, 0xFF, 0, 0, 0)
        blob = iwi.dds_to_iwi(bytes(header) + bytes([255, 170, 85, 0]))
        info = oat_load_iwi27(blob)
        self.assertEqual(info['fmt'], iwi.FMT_RGBA)
        self.assertEqual(blob[-16:], bytes([255, 255, 255, 255, 170, 170, 170, 255, 85, 85, 85, 255, 0, 0, 0, 255]))


class RgbTests(unittest.TestCase):
    def test_waw_24bit_rgb_images_are_written_as_rgba(self):
        header = bytearray(128)
        header[:4] = b'DDS '
        struct.pack_into('<7I', header, 4, 124, 0x100F, 1, 2, 0, 1, 1)
        struct.pack_into('<2I4s5I', header, 76, 32, 0x40, b'\0\0\0\0', 24, 0xFF0000, 0xFF00, 0xFF, 0)
        blob = iwi.dds_to_iwi(bytes(header) + bytes([10, 20, 30, 40, 50, 60]))   # D3DFMT_R8G8B8: B, G, R
        self.assertEqual(oat_load_iwi27(blob)['fmt'], iwi.FMT_RGBA)
        self.assertEqual(blob[-8:], bytes([30, 20, 10, 255, 60, 50, 40, 255]))

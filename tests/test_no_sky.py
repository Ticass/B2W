import tempfile
import unittest
from pathlib import Path

from waw2bo2 import iwi, t6bridge


class NoSkyTests(unittest.TestCase):
    def test_black_cube_converts_like_a_waw_sky_cube(self):
        data = t6bridge.black_cube_dds(4, t6bridge.SKY_HDR_ALPHA)
        self.assertEqual(len(data), 128 + 6 * 4 * 4 * 4)
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = Path(tmp) / "sky.dds", Path(tmp) / "sky.iwi"
            src.write_bytes(data)
            iwi.convert_file(src, dst)
            self.assertEqual(dst.read_bytes()[:3], b"IWi")


class PlainIwiTests(unittest.TestCase):
    def test_dxt1_iwi6_becomes_dds_largest_mip_first(self):
        import struct
        sizes = [iwi.mip_size(iwi.FMT_DXT1, 16, 16, 1, level) for level in range(4)]   # 16..2
        mips = [bytes([level]) * size for level, size in enumerate(sizes)]
        body = b"".join(reversed(mips))                                                 # stored smallest first
        blob = b"IWi\x06" + struct.pack("<BB3H", iwi.FMT_DXT1, 0, 16, 16, 1) + struct.pack("<4I", 0, 0, 0, 0) + body
        dds = iwi.iwi6_to_dds(blob)
        image = iwi.read_dds(dds)
        self.assertEqual((image.width, image.height, image.iwi_format, image.mip_count), (16, 16, iwi.FMT_DXT1, 4))
        self.assertEqual(dds[128:128 + sizes[0]], mips[0])
        iwi.dds_to_iwi(dds)                                                              # converts like any DDS

    def test_wavelet_iwi6_is_left_to_the_wavelet_decoder(self):
        import struct
        blob = b"IWi\x06" + struct.pack("<BB3H", 6, 0, 16, 16, 1) + bytes(16) + bytes(64)
        self.assertIsNone(iwi.iwi6_to_dds(blob))


class NanTransformTests(unittest.TestCase):
    def test_null_joint_offsets_become_neutral(self):
        gltf = {"nodes": [{"name": "joint1", "translation": [None, None, None]},
                          {"name": "root", "translation": [1.0, 2.0, 3.0], "rotation": [0, 0, 0, 1]}]}
        self.assertEqual(t6bridge._neutralize_nan_transforms(gltf), 1)
        self.assertEqual(gltf["nodes"][0]["translation"], [0.0, 0.0, 0.0])
        self.assertEqual(gltf["nodes"][1]["translation"], [1.0, 2.0, 3.0])


if __name__ == "__main__":
    unittest.main()

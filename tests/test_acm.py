import os
import struct
import unittest

from waw2bo2 import acm


class NativeAcmTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "requires the native Windows codec")
    def test_mono_adpcm_known_block(self):
        coefficients = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
        fmt = struct.pack("<HHIIHHHHH", 2, 1, 8000, 16000, 8, 4, 32, 4, 7)
        fmt += b"".join(struct.pack("<hh", *pair) for pair in coefficients)
        # predictor0, delta16, initial samples0,0; nibble +1 then -1.
        block = struct.pack("<BhhhB", 0, 16, 0, 0, 0x1f)
        output = acm.decode(fmt, block)
        self.assertEqual(struct.unpack("<4h", output), (0, 0, 16, 0))

    @unittest.skipUnless(os.name == "nt", "requires the native Windows codec")
    def test_stereo_adpcm_channel_order(self):
        coefficients = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
        fmt = struct.pack("<HHIIHHHHH", 2, 2, 8000, 40000, 15, 4, 32, 3, 7)
        fmt += b"".join(struct.pack("<hh", *pair) for pair in coefficients)
        # Separate initial histories, high nibble left, low nibble right.
        block = bytes([0, 0]) + struct.pack("<6h", 16, 16, 100, 200, 90, 190) + bytes([0x1f])
        self.assertEqual(struct.unpack("<6h", acm.decode(fmt, block)), (90, 190, 100, 200, 116, 184))

    @unittest.skipUnless(os.name == "nt", "requires the native Windows codec")
    def test_negative_prediction_rounds_down_not_toward_zero(self):
        coefficients = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
        fmt = struct.pack("<HHIIHHHHH", 2, 1, 8000, 16000, 8, 4, 32, 4, 7)
        fmt += b"".join(struct.pack("<hh", *pair) for pair in coefficients)
        block = struct.pack("<BhhhB", 3, 16, 1, -6, 0)
        self.assertEqual(struct.unpack("<4h", acm.decode(fmt, block)), (-6, 1, -1, -1))


if __name__ == "__main__":
    unittest.main()

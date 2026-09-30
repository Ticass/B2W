import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from waw2bo2 import xwma
from waw2bo2.sounds import SoundError


def parts():
    return {b"fmt ": struct.pack("<HHIIHHH", 0x161, 1, 44100, 6000, 4, 16, 0),
            b"data": b"abcdefgh", b"dpds": struct.pack("<II", 4, 8)}


class XwmaTests(unittest.TestCase):
    def test_source_packet_counts_not_guessed_duration(self):
        self.assertEqual(xwma.validate(parts()), (8, 1))
        source = parts()
        source[b"dpds"] = struct.pack("<II", 8, 4)
        with self.assertRaises(SoundError):
            xwma.validate(source)
        source = parts()
        source[b"data"] += b"x"
        with self.assertRaises(SoundError):
            xwma.validate(source)

    def test_invalid_profile_and_half_pcm_frame_fail(self):
        source = parts()
        source[b"fmt "] = struct.pack("<HHIIHHH", 0x161, 6, 44100, 6000, 4, 16, 0)
        with self.assertRaises(SoundError):
            xwma.validate(source)
        source = parts()
        source[b"dpds"] = struct.pack("<II", 4, 7)
        with self.assertRaises(SoundError):
            xwma.validate(source)

    def test_unverified_codec_never_called(self):
        with tempfile.TemporaryDirectory() as temp:
            dll, helper = Path(temp) / "codec.dll", Path(temp) / "helper.exe"
            dll.write_bytes(b"not audited")
            helper.write_bytes(b"not invoked")
            with patch.object(xwma, "system_codec", return_value=dll), patch.object(xwma.subprocess, "run") as run:
                with self.assertRaisesRegex(SoundError, "not audited"):
                    xwma.decode(Path(temp) / "original.xwma", parts(), helper)
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()

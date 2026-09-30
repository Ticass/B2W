import struct
import tempfile
import unittest
import os
from pathlib import Path

from waw2bo2 import audio
from waw2bo2.sounds import SoundError


class AudioTests(unittest.TestCase):
    def source(self, directory, rate=44100):
        payload = struct.pack("<hhhh", -32768, -1, 0, 32767)
        wav = bytearray(audio.canonical_pcm(payload, 1, 44100))
        struct.pack_into("<I", wav, 24, rate)
        struct.pack_into("<I", wav, 28, rate * 2)
        private = b"PRIV" + struct.pack("<I", 4) + b"meta"
        wav = wav[:36] + private + wav[36:]
        struct.pack_into("<I", wav, 4, len(wav) - 8)
        path = Path(directory) / "original.wav"
        path.write_bytes(wav)
        return path, payload

    def test_extra_chunks_never_become_pcm_samples(self):
        with tempfile.TemporaryDirectory() as temp:
            source, payload = self.source(temp)
            wav, report = audio.convert(source)
            self.assertEqual(len(wav), 44 + len(payload))
            self.assertEqual(wav[44:], payload)
            self.assertEqual(report["frames"], 4)
            self.assertEqual(report["status"], "exact_original_pcm")

    def test_bytes_after_declared_riff_are_not_audio(self):
        with tempfile.TemporaryDirectory() as temp:
            source, payload = self.source(temp)
            source.write_bytes(source.read_bytes() + b"outside RIFF object")
            wav, report = audio.convert(source)
            self.assertEqual(wav[44:], payload)
            self.assertEqual(report["riff_trailing_bytes"], len(b"outside RIFF object"))

    def test_non_enum_rate_requires_explicit_alias_pitch_translation(self):
        with tempfile.TemporaryDirectory() as temp:
            source, payload = self.source(temp, 43999)
            wav, report = audio.convert(source)
            self.assertEqual(wav[44:], payload)
            self.assertEqual(report["bank_sample_rate"], 44100)
            self.assertEqual(report["alias_pitch_scale"], 43999 / 44100)
            self.assertIn("sample_rate_translation", report)

    def test_truncated_riff_and_unsupported_channels_fail(self):
        with self.assertRaises(SoundError):
            audio.chunks(b"RIFF\x00\x00\x00\x00WAVE")
        with self.assertRaises(SoundError):
            audio.canonical_pcm(b"", 6, 44100)
        with self.assertRaises(SoundError):
            audio.canonical_pcm(b"x", 1, 44100)

    @unittest.skipUnless(os.name == "nt", "native ADPCM needs Windows")
    def test_conversion_uses_xaudio_matching_negative_rounding(self):
        coefficients = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
        fmt = struct.pack("<HHIIHHHHH", 2, 1, 8000, 16000, 8, 4, 32, 4, 7)
        fmt += b"".join(struct.pack("<hh", *pair) for pair in coefficients)
        block = struct.pack("<BhhhB", 3, 16, 1, -6, 0)
        body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(block)) + block
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "source.wav"
            path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
            wav, report = audio.convert(path)
            self.assertEqual(report["status"], "native_ms_adpcm")
            self.assertEqual(struct.unpack("<4h", wav[44:]), (-6, 1, -1, -1))


if __name__ == "__main__":
    unittest.main()

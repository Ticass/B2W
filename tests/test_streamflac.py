import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from waw2bo2 import audio, native, streamflac


class StreamFlacTests(unittest.TestCase):
    def test_shared_source_keeps_loaded_wav_and_preserves_streamed_samples(self):
        encoder = audio.find_decoder()
        if native.tool() is None or encoder is None:
            self.skipTest("native converter and FFmpeg required")
        pcm = struct.pack("<hh", 1234, -1234) * 12000
        with tempfile.TemporaryDirectory() as directory:
            pcm_root = Path(directory)
            wav = pcm_root / "sound.wav"
            source = audio.canonical_pcm(pcm, 1, 24000)
            wav.write_bytes(source)
            (pcm_root / "stream.wav").write_bytes(source)
            rows = [{"Storage": "loaded", "FileSource": "sound.wav"},
                    {"Storage": "streamed", "FileSource": "sound.wav"},
                    {"Storage": "streamed", "FileSource": "stream.wav"}]
            result = streamflac.compress_rows(rows, pcm_root)

            self.assertEqual(wav.read_bytes(), source)
            self.assertEqual(result["files"], 1)
            self.assertEqual(result["original_bytes"], len(pcm))
            self.assertLess(result["output_bytes"], len(pcm))
            self.assertEqual([row["FileSource"] for row in rows], ["sound.wav", "sound.wav", "stream.flac"])
            encoded = pcm_root / "stream.flac"
            decoded = subprocess.run([str(encoder), "-v", "error", "-i", str(encoded),
                                      "-f", "s16le", "-c:a", "pcm_s16le", "pipe:1"],
                                     check=True, capture_output=True).stdout
            self.assertEqual(decoded, pcm)

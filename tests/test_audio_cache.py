import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from waw2bo2 import audio, audiocache


class AudioCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        patcher = mock.patch.dict(os.environ, {'WAW2BO2_AUDIO_CACHE_DIR': str(root / 'cache'),
                                               'WAW2BO2_AUDIO_CACHE': '1'})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.project = root / 'project'
        (self.project / 'audio').mkdir(parents=True)

    def entry(self, name, rate=48000):
        payload = struct.pack('<hhhh', -32768, -1, 0, 32767)
        wav = bytearray(audio.canonical_pcm(payload, 1, 48000))
        struct.pack_into('<I', wav, 24, rate)
        struct.pack_into('<I', wav, 28, rate * 2)
        (self.project / 'audio' / f'{name}.wav').write_bytes(bytes(wav))
        return {'name': name, 'loadType': 'loaded', 'output': f'audio/{name}.wav', 'aliases': ['a']}

    def run_twice(self, entry):
        task = (self.project, entry, None, None)
        first = audio._convert_audio(task)
        with mock.patch.object(audio, '_decode', side_effect=AssertionError('decoded again')):
            second = audio._convert_audio(task)
        return first, second

    def test_hit_returns_the_same_pcm_and_report_order(self):
        first, second = self.run_twice(self.entry('plain'))
        self.assertIsNone(first[3])
        self.assertEqual(first[1], second[1])
        self.assertEqual(list(first[2].items()), list(second[2].items()))
        keys = list(first[2])
        self.assertEqual(keys[-3:], ['name', 'loadType', 'aliases'])

    def test_resample_keys_follow_name_and_load_type(self):
        # the report lists the resample's own keys after name/loadType, as before
        resampled = audio.canonical_pcm(b'\0\0' * 4, 1, 48000)

        def fake_resample(pcm, channels, rate, target, tool):
            return resampled[44:]

        with mock.patch.object(audio, 'find_decoder', return_value=Path('ffmpeg.exe')), \
                mock.patch.object(audio, 'resample', side_effect=fake_resample):
            first, second = self.run_twice(self.entry('slow', rate=44100))
        self.assertIsNone(first[3], first[3])
        keys = list(first[2])
        self.assertLess(keys.index('loadType'), keys.index('original_pcm_sha256'))
        self.assertEqual(keys[-1], 'aliases')
        self.assertEqual(list(first[2].items()), list(second[2].items()))
        self.assertEqual(first[1], second[1])

    def test_changed_source_misses(self):
        entry = self.entry('plain')
        audio._convert_audio((self.project, entry, None, None))
        path = self.project / 'audio/plain.wav'
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(bytes(data))
        decoded = []
        real = audio._decode

        def spy(*args):
            decoded.append(args)
            return real(*args)

        with mock.patch.object(audio, '_decode', side_effect=spy):
            audio._convert_audio((self.project, entry, None, None))
        self.assertEqual(len(decoded), 1)

    def test_disabled_cache_always_decodes(self):
        entry = self.entry('plain')
        with mock.patch.dict(os.environ, {'WAW2BO2_AUDIO_CACHE': '0'}):
            audio._convert_audio((self.project, entry, None, None))
            self.assertFalse((Path(self.temp.name) / 'cache').exists())


if __name__ == '__main__':
    unittest.main()

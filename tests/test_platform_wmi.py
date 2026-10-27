import platform
import unittest
from unittest import mock

import waw2bo2  # noqa: F401  (hardens platform on import)


class PlatformWmiTests(unittest.TestCase):
    def test_a_wmi_answer_without_the_field_takes_the_environment_fallback(self):
        # Subway (Rizuli): KeyError: 'Architecture' from imageio_ffmpeg's
        # platform.machine() at audio decoding, on a PC with a damaged WMI
        class BrokenWmi:
            @staticmethod
            def exec_query(query):
                return ""
        with mock.patch.object(platform, "_wmi", BrokenWmi, create=True), \
                mock.patch.dict("os.environ", {"PROCESSOR_ARCHITECTURE": "AMD64"}):
            with self.assertRaises(OSError):
                platform._wmi_query("CPU", "Architecture")
            if hasattr(platform, "_get_machine_win32"):
                self.assertEqual(platform._get_machine_win32(), "AMD64")


if __name__ == "__main__":
    unittest.main()

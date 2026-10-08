import unittest

from map_compatibility import (
    decode_report_marker,
    encode_report_marker,
    normalize_map,
    search_maps,
    valid_video_url,
    validate_upload,
)


class MapCompatibilityTests(unittest.TestCase):
    def test_normalizes_codrepo_api_record_and_rejects_other_hosts(self):
        row = {"id": 17, "title": {"rendered": "<b>Test &amp; Map</b>"},
               "link": "https://callofdutyrepo.com/maps/test", "slug": "test"}
        self.assertEqual(normalize_map(row)["title"], "Test & Map")
        row["link"] = "https://example.org/map"
        self.assertIsNone(normalize_map(row))

    def test_search_prioritizes_prefix_and_limits_to_discord_choices(self):
        maps = [{"id": 1, "title": "Nazi Zombies"}, {"id": 2, "title": "Classic Nazi"},
                {"id": 3, "title": "Nazi Redux"}]
        self.assertEqual([m["id"] for m in search_maps(maps, "nazi")], [1, 3, 2])
        self.assertEqual(len(search_maps(maps, "", limit=2)), 2)

    def test_marker_round_trip_and_validation(self):
        report = {"map_id": 7, "map_title": "Map", "outcome": "broken",
                  "platform": "linux", "discord_url": "https://discord.com/channels/1/2/3"}
        self.assertEqual(decode_report_marker(encode_report_marker(report)), report)
        self.assertIsNone(decode_report_marker("<!-- invalid -->"))

    def test_video_and_upload_validation(self):
        self.assertEqual(valid_video_url("https://video.example/watch"), "https://video.example/watch")
        with self.assertRaises(ValueError):
            valid_video_url("javascript:alert(1)")
        self.assertIsNone(validate_upload("screen.PNG", 1024))
        self.assertIsNotNone(validate_upload("game.exe", 1024))
        self.assertIsNotNone(validate_upload("crash.dmp", 9 * 1024 * 1024))


if __name__ == "__main__":
    unittest.main()
